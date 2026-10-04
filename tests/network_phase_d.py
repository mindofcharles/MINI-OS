#!/usr/bin/env python3
"""QEMU acceptance for application-linked ARP and ICMP Echo."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import socket
import struct
import sys
import tempfile
from typing import Optional

from qemu_e2e import VirtualMachine, check_image, prepare_debug_image
from qemu_packet_socket import (
    connect_peer, network_args, recv_frame, reserve_port, send_frame,
)


GUEST_MAC = bytes.fromhex("52 54 00 12 34 56")
PEER_MAC = bytes.fromhex("02 00 00 00 00 01")
BARRIER_MAC = bytes.fromhex("02 00 00 00 00 03")
BAD_MAC = bytes.fromhex("02 00 00 00 00 09")
BROADCAST_MAC = b"\xff" * 6
GUEST_IP = bytes((10, 0, 2, 15))
PEER_IP = bytes((10, 0, 2, 2))
BARRIER_IP = bytes((10, 0, 2, 3))
SHELL_PROMPT = "/ > "
PING_PAYLOAD = b"MINI-OS ICMP Echo"
PROBE_PAYLOAD = b"D6"
OPERATION_TIMEOUT = 16.0


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def checksum(data: bytes) -> int:
    if len(data) & 1:
        data += b"\x00"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total > 0xFFFF:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def ethernet(destination: bytes, source: bytes, kind: int,
             payload: bytes, padding: int = 0) -> bytes:
    frame = destination + source + struct.pack("!H", kind) + payload
    return frame.ljust(60, bytes((padding,)))


def arp_frame(operation: int, destination: bytes, source: bytes,
              sender_ip: bytes, target_mac: bytes, target_ip: bytes,
              sender_mac: Optional[bytes] = None) -> bytes:
    sender = source if sender_mac is None else sender_mac
    payload = struct.pack("!HHBBH", 1, 0x0800, 6, 4, operation)
    payload += sender + sender_ip + target_mac + target_ip
    return ethernet(destination, source, 0x0806, payload)


def expected_arp_request() -> bytes:
    return arp_frame(1, BROADCAST_MAC, GUEST_MAC, GUEST_IP,
                     b"\x00" * 6, PEER_IP)


def send_arp_reply(peer: socket.socket) -> None:
    send_frame(peer, arp_frame(2, GUEST_MAC, PEER_MAC, PEER_IP,
                               GUEST_MAC, GUEST_IP))


def expect_arp_request(peer: socket.socket) -> None:
    actual = recv_frame(peer)
    require(actual == expected_arp_request(),
            f"guest ARP Request differs from exact 60-byte wire image: {actual.hex()}")


def echo_frame(destination_mac: bytes, source_mac: bytes,
               source_ip: bytes, destination_ip: bytes, kind: int,
               identifier: int, sequence: int, payload: bytes,
               padding: int = 0) -> bytes:
    message = struct.pack("!BBHHH", kind, 0, 0, identifier, sequence) + payload
    message = message[:2] + struct.pack("!H", checksum(message)) + message[4:]
    header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(message),
                         0, 0x4000, 64, 1, 0, source_ip, destination_ip)
    header = header[:10] + struct.pack("!H", checksum(header)) + header[12:]
    return ethernet(destination_mac, source_mac, 0x0800,
                    header + message, padding)


def expect_echo(peer: socket.socket, sequence: int,
                payload: bytes, kind: int = 8) -> int:
    actual = recv_frame(peer)
    require(len(actual) >= 42 and actual[12:14] == b"\x08\x00",
            f"expected Echo sequence {sequence}, got {actual.hex()}")
    identifier = struct.unpack("!H", actual[38:40])[0]
    expected = echo_frame(PEER_MAC, GUEST_MAC, GUEST_IP, PEER_IP,
                          kind, identifier, sequence, payload)
    require(actual == expected,
            f"Echo sequence {sequence} differs from exact wire image: "
            f"expected {expected.hex()}, got {actual.hex()}")
    return identifier


def send_echo_reply(peer: socket.socket, identifier: int, sequence: int,
                    payload: bytes, padding: int = 0) -> None:
    send_frame(peer, echo_frame(GUEST_MAC, PEER_MAC, PEER_IP, GUEST_IP,
                                0, identifier, sequence, payload, padding))


def patch_ip_word(frame: bytes, offset: int, value: int) -> bytes:
    changed = bytearray(frame)
    struct.pack_into("!H", changed, 14 + offset, value)
    changed[24:26] = b"\x00\x00"
    struct.pack_into("!H", changed, 24, checksum(bytes(changed[14:34])))
    return bytes(changed)


def inject_rejected_packets(peer: socket.socket, identifier: int) -> None:
    valid = echo_frame(GUEST_MAC, PEER_MAC, PEER_IP, GUEST_IP,
                       0, identifier, 1, PING_PAYLOAD)
    wrong_type = bytearray(valid)
    wrong_type[12:14] = b"\x88\xb5"
    bad_ip = bytearray(valid)
    bad_ip[24] ^= 1
    bad_icmp = bytearray(valid)
    bad_icmp[36] ^= 1
    rejected = (
        bytes(wrong_type),
        bytes(bad_ip),
        bytes(bad_icmp),
        patch_ip_word(valid, 2, 100),
        patch_ip_word(valid, 2, 27),
        patch_ip_word(valid, 6, 0x6000),
        echo_frame(GUEST_MAC, PEER_MAC, PEER_IP, GUEST_IP,
                   0, identifier ^ 1, 1, PING_PAYLOAD),
        echo_frame(GUEST_MAC, PEER_MAC, PEER_IP, GUEST_IP,
                   0, identifier, 2, PING_PAYLOAD),
        echo_frame(GUEST_MAC, PEER_MAC, PEER_IP, GUEST_IP,
                   0, identifier, 1, b"WRONG"),
        echo_frame(GUEST_MAC, PEER_MAC, BARRIER_IP, GUEST_IP,
                   0, identifier, 1, PING_PAYLOAD),
        arp_frame(2, GUEST_MAC, BAD_MAC, PEER_IP, GUEST_MAC, GUEST_IP),
    )
    for frame in rejected:
        send_frame(peer, frame)


def expect_shell(vm: VirtualMachine, start: int, marker: str) -> str:
    segment = vm.wait_for(SHELL_PROMPT, start, OPERATION_TIMEOUT)
    require(marker in segment, f"guest omitted {marker!r}:\n{segment}")
    vm.command_expect("pwd", SHELL_PROMPT)
    return segment


def ping_with_faults(vm: VirtualMachine, peer: socket.socket) -> None:
    start = vm.send_command("run /transport/build/apps/ping.bin 10.0.2.2")
    expect_arp_request(peer)
    send_arp_reply(peer)
    identifier = expect_echo(peer, 1, PING_PAYLOAD)
    inject_rejected_packets(peer, identifier)
    send_frame(peer, arp_frame(1, BROADCAST_MAC, BARRIER_MAC,
                               BARRIER_IP, b"\x00" * 6, GUEST_IP))
    barrier_reply = recv_frame(peer)
    expected_barrier = arp_frame(2, BARRIER_MAC, GUEST_MAC, GUEST_IP,
                                 BARRIER_MAC, BARRIER_IP)
    require(barrier_reply == expected_barrier,
            "guest did not process rejected packets before the ARP barrier")
    require("17 bytes from" not in vm.text()[start:],
            "a rejected packet completed Ping before the valid reply")
    send_echo_reply(peer, identifier, 1, PING_PAYLOAD, padding=0xA5)
    for sequence in (2, 3, 4):
        identifier = expect_echo(peer, sequence, PING_PAYLOAD)
        send_echo_reply(peer, identifier, sequence, PING_PAYLOAD)
    segment = expect_shell(vm, start, "ping summary: 4 sent, 4 received, 0 lost")
    for sequence in (1, 2, 3, 4):
        require(f"seq={sequence} time=" in segment,
                f"Ping omitted successful sequence {sequence}:\n{segment}")


def cache_expiry(vm: VirtualMachine, peer: socket.socket) -> None:
    start = vm.send_command(
        "run /transport/build/lib_test/test_net_protocol.bin cache"
    )
    expect_arp_request(peer)
    send_arp_reply(peer)
    for sequence in (1, 2):
        identifier = expect_echo(peer, sequence, PROBE_PAYLOAD)
        send_echo_reply(peer, identifier, sequence, PROBE_PAYLOAD)
    vm.wait_for("D6 CACHE SECOND: PASS", start, OPERATION_TIMEOUT)
    expect_arp_request(peer)
    send_arp_reply(peer)
    identifier = expect_echo(peer, 3, PROBE_PAYLOAD)
    send_echo_reply(peer, identifier, 3, PROBE_PAYLOAD)
    segment = expect_shell(vm, start, "D6 CACHE EXPIRED: PASS")
    require("D6 CACHE FIRST: PASS" in segment,
            "first cached Ping did not complete")


def echo_server(vm: VirtualMachine, peer: socket.socket) -> None:
    start = vm.send_command(
        "run /transport/build/lib_test/test_net_protocol.bin server"
    )
    vm.wait_for("D6 SERVER READY", start, OPERATION_TIMEOUT)
    send_frame(peer, arp_frame(1, BROADCAST_MAC, PEER_MAC, PEER_IP,
                               b"\x00" * 6, GUEST_IP))
    actual = recv_frame(peer)
    expected = arp_frame(2, PEER_MAC, GUEST_MAC, GUEST_IP,
                         PEER_MAC, PEER_IP)
    require(actual == expected,
            f"guest ARP Reply differs from exact wire image: {actual.hex()}")
    vm.wait_for("D6 SERVER ARP: PASS", start, OPERATION_TIMEOUT)
    payload = b"guest-echo"
    identifier = 0xD601
    sequence = 7
    send_frame(peer, echo_frame(GUEST_MAC, PEER_MAC, PEER_IP, GUEST_IP,
                                8, identifier, sequence, payload,
                                padding=0xA5))
    expect_echo(peer, sequence, payload, kind=0)
    expect_shell(vm, start, "D6 SERVER ECHO: PASS")


def arp_timeout(vm: VirtualMachine, peer: socket.socket) -> None:
    start = vm.send_command(
        "run /transport/build/lib_test/test_net_protocol.bin arp_timeout"
    )
    expect_arp_request(peer)
    expect_arp_request(peer)
    expect_shell(vm, start, "D6 ARP TIMEOUT: PASS")


def icmp_timeout(vm: VirtualMachine, peer: socket.socket) -> None:
    start = vm.send_command(
        "run /transport/build/lib_test/test_net_protocol.bin icmp_timeout"
    )
    expect_arp_request(peer)
    send_arp_reply(peer)
    expect_echo(peer, 1, PROBE_PAYLOAD)
    expect_shell(vm, start, "D6 ICMP TIMEOUT: PASS")


def cancellation(vm: VirtualMachine, peer: socket.socket) -> None:
    start = vm.send_command(
        "run /transport/build/lib_test/test_net_protocol.bin cancel"
    )
    expect_arp_request(peer)
    send_arp_reply(peer)
    expect_echo(peer, 1, PROBE_PAYLOAD)
    vm.hmp("sendkey c 50")
    expect_shell(vm, start, "D6 CANCEL: PASS")


def deterministic_peer_test(repo: Path, source: Path, qemu: str,
                            temp: Path, artifacts: list[Path]) -> None:
    image = temp / "network-phase-d-peer.img"
    capture = temp / "network-phase-d-peer.pcap"
    log = temp / "network-phase-d-peer.log"
    artifacts.extend((capture, log))
    port = reserve_port()
    prepare_debug_image(repo, source, image)
    vm = VirtualMachine(image, log, qemu, 200,
                        extra_args=network_args(port, capture))
    peer: Optional[socket.socket] = None
    try:
        vm.start()
        peer = connect_peer(port)
        peer.settimeout(OPERATION_TIMEOUT)
        ping_with_faults(vm, peer)
        cache_expiry(vm, peer)
        echo_server(vm, peer)
        arp_timeout(vm, peer)
        icmp_timeout(vm, peer)
        cancellation(vm, peer)
    finally:
        if peer is not None:
            peer.close()
        vm.stop()
    check_image(repo / "build/check_image", image, repo)


def user_network_test(repo: Path, source: Path, qemu: str,
                      temp: Path, artifacts: list[Path]) -> None:
    image = temp / "network-phase-d-user.img"
    log = temp / "network-phase-d-user.log"
    artifacts.append(log)
    prepare_debug_image(repo, source, image)
    vm = VirtualMachine(
        image, log, qemu, 201,
        extra_args=[
            "-accel", "tcg", "-cpu", "max,rdrand=on",
            "-netdev", "user,id=net0",
            "-device",
            "ne2k_isa,netdev=net0,iobase=0x300,irq=9,"
            "mac=52:54:00:12:34:56",
        ],
    )
    try:
        vm.start()
        start = vm.send_command("run /transport/build/apps/ping.bin 10.0.2.2")
        segment = expect_shell(vm, start,
                               "ping summary: 4 sent, 4 received, 0 lost")
        for sequence in (1, 2, 3, 4):
            require(f"seq={sequence} time=" in segment,
                    f"QEMU user-network Ping omitted sequence {sequence}")
    finally:
        vm.stop()
    check_image(repo / "build/check_image", image, repo)


def retain_failure_artifacts(repo: Path, artifacts: list[Path]) -> None:
    destination = repo / "build/test-artifacts"
    destination.mkdir(parents=True, exist_ok=True)
    failure_dir: Optional[Path] = None
    for artifact in artifacts:
        if artifact.is_file() and artifact.stat().st_size != 0:
            if failure_dir is None:
                failure_dir = Path(tempfile.mkdtemp(
                    prefix="network-phase-d-failure-", dir=destination
                ))
            name = artifact.stem + "-failure" + artifact.suffix
            shutil.copy2(artifact, failure_dir / name)
    if failure_dir is not None:
        print(f"Phase D failure artifacts retained in {failure_dir}",
              file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--qemu", default="qemu-system-i386")
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[1]
    source = args.image.resolve()
    if not source.is_file():
        parser.error("--image must name an existing image")
    artifacts: list[Path] = []
    with tempfile.TemporaryDirectory(prefix="mini-os-network-d-") as directory:
        temp = Path(directory)
        try:
            deterministic_peer_test(repo, source, args.qemu, temp, artifacts)
            user_network_test(repo, source, args.qemu, temp, artifacts)
        except Exception:
            retain_failure_artifacts(repo, artifacts)
            raise
    print("ARP and ICMP QEMU acceptance: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"ARP and ICMP QEMU acceptance: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
