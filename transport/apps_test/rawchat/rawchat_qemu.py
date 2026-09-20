#!/usr/bin/env python3
"""QEMU full-duplex and reconnect regression for Raw Ethernet Chat."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import socket
import struct
import sys
import tempfile
import time
from typing import Optional


REPOSITORY = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests"))
sys.path.insert(0, str(REPOSITORY / "host_apps/rawchat"))

import rawchat_peer as rawchat
from qemu_e2e import VirtualMachine, check_image, prepare_debug_image


OPERATION_TIMEOUT = 12.0


def listen_on_free_port() -> tuple[socket.socket, int]:
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", 0))
    server.listen(2)
    return server, int(server.getsockname()[1])


def accept_with_deadline(server: socket.socket, timeout: float) -> socket.socket:
    server.settimeout(timeout)
    try:
        connection, _ = server.accept()
    except socket.timeout as error:
        raise RuntimeError("timed out waiting for QEMU stream connection") from error
    connection.settimeout(timeout)
    return connection


def receive_exact(connection: socket.socket, length: int) -> bytes:
    data = bytearray()
    while len(data) < length:
        try:
            chunk = connection.recv(length - len(data))
        except socket.timeout as error:
            raise RuntimeError(
                f"timed out after receiving {len(data)} of {length} stream bytes"
            ) from error
        if not chunk:
            raise RuntimeError("QEMU stream closed unexpectedly")
        data.extend(chunk)
    return bytes(data)


def receive_frame(connection: socket.socket) -> bytes:
    length = struct.unpack("!I", receive_exact(connection, 4))[0]
    if not rawchat.FRAME_MIN <= length <= rawchat.FRAME_MAX:
        raise RuntimeError(f"QEMU emitted invalid Ethernet length {length}")
    return receive_exact(connection, length)


def receive_messages(
    connection: socket.socket,
    message_type: int,
    count: int,
    timeout: float = OPERATION_TIMEOUT,
) -> list[rawchat.Message]:
    messages: list[rawchat.Message] = []
    deadline = time.monotonic() + timeout
    while len(messages) < count:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError("timed out waiting for Raw Chat messages")
        connection.settimeout(remaining)
        frame = receive_frame(connection)
        try:
            message = rawchat.parse_frame(
                frame, rawchat.DEFAULT_PEER_MAC, rawchat.DEFAULT_MINI_MAC
            )
        except rawchat.UnrelatedFrame:
            continue
        if message.message_type == message_type:
            messages.append(message)
    return messages


def host_message(session: int, sequence: int, text: bytes) -> bytes:
    return rawchat.encode_stream_frame(
        rawchat.build_frame(
            rawchat.DEFAULT_PEER_MAC,
            rawchat.DEFAULT_MINI_MAC,
            rawchat.Message(rawchat.MESSAGE_TEXT, session, sequence, text),
        )
    )


def host_hello(session: int) -> bytes:
    return rawchat.encode_stream_frame(
        rawchat.build_frame(
            rawchat.DEFAULT_PEER_MAC,
            rawchat.DEFAULT_MINI_MAC,
            rawchat.Message(rawchat.MESSAGE_HELLO, session, 0, b""),
        )
    )


def network_args(port: int) -> list[str]:
    return [
        "-accel",
        "tcg",
        "-cpu",
        "max,rdrand=on",
        "-netdev",
        "stream,id=net0,server=off,addr.type=inet,"
        f"addr.host=127.0.0.1,addr.port={port},reconnect-ms=1000",
        "-device",
        "ne2k_isa,netdev=net0,iobase=0x300,irq=9,mac=52:54:00:12:34:56",
    ]


def run_test(
    repo: Path,
    source: Path,
    checker: Path,
    qemu: str,
    temp: Path,
) -> None:
    image = temp / "rawchat.img"
    log = temp / "rawchat.log"
    prepare_debug_image(repo, source, image)
    server, port = listen_on_free_port()
    vm = VirtualMachine(
        image,
        log,
        qemu,
        120,
        extra_args=network_args(port),
    )
    connection: Optional[socket.socket] = None
    try:
        vm.start()
        connection = accept_with_deadline(server, 5.0)
        start = vm.send_command("run /transport/build/apps/rawchat.bin")
        vm.wait_for("MINI-OS Raw Ethernet Chat", start, OPERATION_TIMEOUT)
        initial_hello = receive_messages(
            connection, rawchat.MESSAGE_HELLO, 1
        )[0]

        first_host_session = 0x01020304
        connection.sendall(
            host_hello(first_host_session)
            + host_message(first_host_session, 1, b"host-one")
            + host_message(first_host_session, 2, b"host-two")
        )
        vm.wait_for("Peer: host-one", start, OPERATION_TIMEOUT)
        vm.wait_for("Peer: host-two", start, OPERATION_TIMEOUT)

        vm.send_command("guest-one")
        vm.send_command("guest-two")
        guest_messages = receive_messages(
            connection, rawchat.MESSAGE_TEXT, 2
        )
        if [message.payload for message in guest_messages] != [
            b"guest-one",
            b"guest-two",
        ]:
            raise RuntimeError("MINI-OS consecutive messages changed or reordered")
        if any(
            message.session_id != initial_hello.session_id
            for message in guest_messages
        ) or [message.sequence for message in guest_messages] != [1, 2]:
            raise RuntimeError("MINI-OS session sequence is invalid")

        malformed = bytearray(
            rawchat.build_frame(
                rawchat.DEFAULT_PEER_MAC,
                rawchat.DEFAULT_MINI_MAC,
                rawchat.Message(
                    rawchat.MESSAGE_TEXT,
                    first_host_session,
                    3,
                    b"bad-version",
                ),
            )
        )
        malformed[15] += 1
        connection.sendall(
            rawchat.encode_stream_frame(bytes(malformed))
            + host_message(first_host_session, 3, b"host-three")
            + host_message(first_host_session, 3, b"host-three")
            + host_message(first_host_session, 5, b"host-gap")
        )
        vm.wait_for("Peer: host-three", start, OPERATION_TIMEOUT)
        vm.wait_for("One or more peer messages were lost.", start,
                    OPERATION_TIMEOUT)
        vm.wait_for("Peer: host-gap", start, OPERATION_TIMEOUT)

        connection.close()
        connection = accept_with_deadline(server, 5.0)
        second_host_session = 0x05060708
        connection.sendall(
            host_hello(second_host_session)
            + host_message(second_host_session, 1, b"after-reconnect")
        )
        vm.wait_for("Peer: after-reconnect", start, OPERATION_TIMEOUT)

        vm.send_command("guest-three")
        after_reconnect = receive_messages(
            connection, rawchat.MESSAGE_TEXT, 1
        )[0]
        if (
            after_reconnect.payload != b"guest-three"
            or after_reconnect.session_id != initial_hello.session_id
            or after_reconnect.sequence != 3
        ):
            raise RuntimeError("MINI-OS did not preserve its session on reconnect")

        exit_start = log.stat().st_size
        vm.hmp("sendkey esc 2")
        vm.wait_for("Raw Ethernet Chat closed.", exit_start, OPERATION_TIMEOUT)
        bye = receive_messages(connection, rawchat.MESSAGE_BYE, 1)[0]
        if bye.session_id != initial_hello.session_id or bye.sequence != 4:
            raise RuntimeError("MINI-OS BYE has an invalid session sequence")
        vm.command_expect("pwd", "/ > ")
    finally:
        if connection is not None:
            connection.close()
        server.close()
        vm.stop()
    check_image(checker, image, repo)


def retain_failure_log(repo: Path, log: Path) -> None:
    if not log.is_file() or log.stat().st_size == 0:
        return
    destination = repo / "build/test-artifacts"
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(log, destination / "rawchat-failure.log")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--checker", required=True, type=Path)
    parser.add_argument("--qemu", default="qemu-system-i386")
    args = parser.parse_args()

    source = args.image.resolve()
    checker = args.checker.resolve()
    if not source.is_file() or not checker.is_file():
        parser.error("--image and --checker must name existing files")

    with tempfile.TemporaryDirectory(prefix="mini-os-rawchat-") as directory:
        temp = Path(directory)
        try:
            run_test(REPOSITORY, source, checker, args.qemu, temp)
        except Exception:
            retain_failure_log(REPOSITORY, temp / "rawchat.log")
            raise

    failure_log = REPOSITORY / "build/test-artifacts/rawchat-failure.log"
    failure_log.unlink(missing_ok=True)
    print("Raw Ethernet Chat QEMU regression: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, socket.timeout) as error:
        print(f"Raw Ethernet Chat QEMU regression: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
