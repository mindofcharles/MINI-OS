#!/usr/bin/env python3
"""Deterministic QEMU regression for the NE2000 raw-frame transport."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import socket
import sys
import tempfile
from typing import Optional

from qemu_e2e import VirtualMachine, check_image, prepare_debug_image
from qemu_packet_socket import (
    FRAME_MIN, connect_peer, network_args, recv_frame, reserve_port, send_frame,
)


MAC_GUEST = bytes.fromhex("52 54 00 12 34 56")
MAC_PEER = bytes.fromhex("02 00 00 00 00 01")
ETHERTYPE_DATA = b"\x88\xb5"
ETHERTYPE_ACK = b"\x88\xb6"
RX_LENGTH = 1000
RX_COUNT = 32
RX_ODD_SEQUENCE = 0xE1
RX_ODD_LENGTH = 61
OPERATION_TIMEOUT = 12.0
FAULT_MARKER = b"MINI_OS_PHASE_C_FAULT_INJECTION_ONLY"
SHELL_PROMPT = "/ > "


def run_network_test(vm: VirtualMachine, mode: str, expected: str) -> str:
    segment = vm.command_expect(
        f"run /transport/build/lib_test/test_network.bin {mode}",
        SHELL_PROMPT,
    )
    if expected not in segment:
        raise RuntimeError(
            f"network test mode {mode!r} omitted {expected!r}:\n{segment}"
        )
    return segment


def tx_frame(length: int, seed: int) -> bytes:
    frame = bytearray((seed + index) & 0xFF for index in range(length))
    frame[0:6] = MAC_PEER
    frame[6:12] = MAC_GUEST
    frame[12:14] = ETHERTYPE_DATA
    frame[14] = seed
    return bytes(frame)


def rx_frame(sequence: int, length: int = RX_LENGTH) -> bytes:
    frame = bytearray((sequence + index) & 0xFF for index in range(length))
    frame[0:6] = MAC_GUEST
    frame[6:12] = MAC_PEER
    frame[12:14] = ETHERTYPE_DATA
    frame[14] = sequence
    return bytes(frame)


def expected_ack(sequence: int) -> bytes:
    frame = bytearray(FRAME_MIN)
    frame[0:6] = MAC_PEER
    frame[6:12] = MAC_GUEST
    frame[12:14] = ETHERTYPE_ACK
    frame[14] = sequence
    return bytes(frame)


def available_transport_test(
    repo: Path, source: Path, qemu: str, temp: Path, artifacts: list[Path]
) -> None:
    image = temp / "network-phase-c.img"
    capture = temp / "network-phase-c.pcap"
    log = temp / "network-phase-c.log"
    artifacts.extend((capture, log))
    port = reserve_port()
    prepare_debug_image(repo, source, image)
    vm = VirtualMachine(
        image,
        log,
        qemu,
        100,
        extra_args=network_args(port, capture),
    )
    peer: Optional[socket.socket] = None
    try:
        vm.start()
        peer = connect_peer(port)
        pic_state = vm.hmp("info pic")
        if (
            re.search(r"imr\s*[:=]\s*(?:0x)?fe", pic_state, re.IGNORECASE)
            is None
            or re.search(
                r"imr\s*[:=]\s*(?:0x)?ff", pic_state, re.IGNORECASE
            )
            is None
        ):
            raise RuntimeError(f"IRQ9 is not masked by the PICs:\n{pic_state}")
        run_network_test(
            vm,
            "info",
            "NETWORK INFO TEST: PASS",
        )

        run_network_test(
            vm,
            "tx",
            "NETWORK TX TEST: PASS",
        )
        for length, seed in ((60, 0x20), (61, 0x21), (1514, 0x22)):
            actual = recv_frame(peer)
            expected = tx_frame(length, seed)
            if actual != expected:
                raise RuntimeError(
                    f"transmit frame {length} changed before reaching the peer"
                )

        start = vm.send_command(
            "run /transport/build/lib_test/test_network.bin rx"
        )
        vm.wait_for("NETWORK RX READY", start, OPERATION_TIMEOUT)
        send_frame(peer, rx_frame(0xF0, 100))
        vm.wait_for("NETWORK RX DROP: PASS", start, OPERATION_TIMEOUT)
        send_frame(peer, rx_frame(RX_ODD_SEQUENCE, RX_ODD_LENGTH))
        acknowledgement = recv_frame(peer)
        if acknowledgement != expected_ack(RX_ODD_SEQUENCE):
            raise RuntimeError("odd-length receive acknowledgement is malformed")
        vm.wait_for("NETWORK RX ODD: PASS", start, OPERATION_TIMEOUT)
        for sequence in range(RX_COUNT):
            send_frame(peer, rx_frame(sequence))
            acknowledgement = recv_frame(peer)
            if acknowledgement != expected_ack(sequence):
                raise RuntimeError(
                    f"receive acknowledgement {sequence} is malformed"
                )
        segment = vm.wait_for(SHELL_PROMPT, start, OPERATION_TIMEOUT)
        if "NETWORK RX WRAP TEST: PASS" not in segment:
            raise RuntimeError(f"receive test did not complete:\n{segment}")
        vm.command_expect("pwd", "/ > ")
    finally:
        if peer is not None:
            peer.close()
        vm.stop()
    check_image(repo / "build/check_image", image, repo)


def unavailable_transport_test(
    repo: Path, source: Path, qemu: str, temp: Path, artifacts: list[Path]
) -> None:
    image = temp / "network-phase-c-unavailable.img"
    log = temp / "network-phase-c-unavailable.log"
    artifacts.append(log)
    prepare_debug_image(repo, source, image)
    vm = VirtualMachine(
        image, log, qemu, 101
    )
    try:
        vm.start()
        run_network_test(
            vm,
            "unavailable",
            "NETWORK UNAVAILABLE TEST: PASS",
        )
    finally:
        vm.stop()

    image = temp / "network-phase-c-wrong-base.img"
    capture = temp / "network-phase-c-wrong-base.pcap"
    log = temp / "network-phase-c-wrong-base.log"
    artifacts.extend((capture, log))
    prepare_debug_image(repo, source, image)
    port = reserve_port()
    vm = VirtualMachine(
        image,
        log,
        qemu,
        102,
        extra_args=network_args(port, capture, io_base=0x320),
    )
    try:
        vm.start()
        run_network_test(
            vm,
            "unavailable",
            "NETWORK UNAVAILABLE TEST: PASS",
        )
    finally:
        vm.stop()


def recovery_test(
    repo: Path, source: Path, qemu: str, temp: Path, artifacts: list[Path]
) -> None:
    image = temp / "network-phase-c-recovery.img"
    capture = temp / "network-phase-c-recovery.pcap"
    log = temp / "network-phase-c-recovery.log"
    artifacts.extend((capture, log))
    prepare_debug_image(
        repo,
        source,
        image,
        kernel_defines=[
            "KERNEL_TEST_NET_OVERRUN_ONCE=1",
            "KERNEL_TEST_NET_DMA_TIMEOUT_ONCE=1",
            "KERNEL_TEST_NET_TX_TIMEOUT_ONCE=1",
        ],
    )
    if FAULT_MARKER not in image.with_suffix(".kernel.bin").read_bytes():
        raise RuntimeError("recovery kernel omitted its test-only fault marker")
    port = reserve_port()
    vm = VirtualMachine(
        image,
        log,
        qemu,
        103,
        extra_args=network_args(port, capture),
    )
    peer: Optional[socket.socket] = None
    try:
        vm.start()
        peer = connect_peer(port)
        run_network_test(
            vm,
            "recovery",
            "NETWORK RECOVERY TEST: PASS",
        )
        recovered = recv_frame(peer)
        if recovered == tx_frame(FRAME_MIN, 0x71):
            recovered = recv_frame(peer)
        if recovered != tx_frame(FRAME_MIN, 0x72):
            raise RuntimeError("post-recovery transmit frame is malformed")
        vm.command_expect("pwd", "/ > ")
    finally:
        if peer is not None:
            peer.close()
        vm.stop()


def fatal_recovery_test(
    repo: Path, source: Path, qemu: str, temp: Path, artifacts: list[Path]
) -> None:
    image = temp / "network-phase-c-fatal.img"
    capture = temp / "network-phase-c-fatal.pcap"
    log = temp / "network-phase-c-fatal.log"
    artifacts.extend((capture, log))
    prepare_debug_image(
        repo,
        source,
        image,
        kernel_defines=[
            "KERNEL_TEST_NET_TX_TIMEOUT_ONCE=1",
            "KERNEL_TEST_NET_RESET_TIMEOUT_ONCE=1",
        ],
    )
    if FAULT_MARKER not in image.with_suffix(".kernel.bin").read_bytes():
        raise RuntimeError("fatal kernel omitted its test-only fault marker")
    port = reserve_port()
    vm = VirtualMachine(
        image,
        log,
        qemu,
        104,
        extra_args=network_args(port, capture),
    )
    peer: Optional[socket.socket] = None
    try:
        vm.start()
        peer = connect_peer(port)
        run_network_test(
            vm,
            "fatal",
            "NETWORK FATAL RECOVERY TEST: PASS",
        )
        vm.command_expect("pwd", "/ > ")
    finally:
        if peer is not None:
            peer.close()
        vm.stop()


def retain_failure_artifacts(repo: Path, artifacts: list[Path]) -> None:
    destination = repo / "build/test-artifacts"
    destination.mkdir(parents=True, exist_ok=True)
    copied = False
    for artifact in artifacts:
        if artifact.is_file() and artifact.stat().st_size != 0:
            failure_name = artifact.stem + "-failure" + artifact.suffix
            shutil.copy2(artifact, destination / failure_name)
            copied = True
    if copied:
        print(f"Phase C failure artifacts retained in {destination}", file=sys.stderr)


def remove_old_failure_artifacts(repo: Path) -> None:
    destination = repo / "build/test-artifacts"
    for artifact in destination.glob("network-phase-c*-failure.*"):
        artifact.unlink()


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
    remove_old_failure_artifacts(repo)
    with tempfile.TemporaryDirectory(prefix="mini-os-network-c-") as directory:
        temp = Path(directory)
        try:
            available_transport_test(repo, source, args.qemu, temp, artifacts)
            unavailable_transport_test(repo, source, args.qemu, temp, artifacts)
            recovery_test(repo, source, args.qemu, temp, artifacts)
            fatal_recovery_test(repo, source, args.qemu, temp, artifacts)
        except Exception:
            retain_failure_artifacts(repo, artifacts)
            raise

    print("NE2000 raw-frame regression: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"NE2000 raw-frame regression: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
