#!/usr/bin/env python3
"""Exercise the public TCP active open against a controlled QEMU wire peer."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import socket
import struct
import sys
import tempfile

from qemu_e2e import VirtualMachine, check_image, prepare_debug_image
from qemu_packet_socket import (
    arp_frame, checksum, connect_peer, ethernet, network_args, recv_frame,
    reserve_port, send_frame,
)


GUEST_MAC = bytes.fromhex("52 54 00 12 34 56")
PEER_MAC = bytes.fromhex("02 00 00 00 00 01")
GUEST_IP = bytes((10, 0, 2, 15))
PEER_IP = bytes((10, 0, 2, 2))
ROUTED_IP = bytes((192, 0, 2, 9))
ROUTED_INGRESS_MAC = bytes.fromhex("02 00 00 00 00 09")
PEER_PORT = 22
PEER_SEQUENCE = 0xFFFFFFFF
SYN, RST, ACK = 0x02, 0x04, 0x10
GUEST_MSS = bytes.fromhex("02 04 02 18")
PEER_MSS = bytes.fromhex("02 04 05 b4")
APP = "run /transport/build/lib_test/test_tcp_contract.bin"
PROMPT = "/ > "
TIMEOUT = 15.0


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def tcp_frame(source_port: int, destination_port: int, sequence: int,
              acknowledgment: int, flags: int, window: int = 0,
              options: bytes = b"", *, payload: bytes = b"", routed: bool = False,
              from_guest: bool = False) -> bytes:
    ingress_mac = ROUTED_INGRESS_MAC if routed else PEER_MAC
    peer_ip = ROUTED_IP if routed else PEER_IP
    source_mac, destination_mac = (GUEST_MAC, PEER_MAC) if from_guest else (
        ingress_mac, GUEST_MAC
    )
    source_ip, destination_ip = (GUEST_IP, peer_ip) if from_guest else (
        peer_ip, GUEST_IP
    )
    offset = (20 + len(options)) // 4 << 4
    segment = struct.pack(
        "!HHIIBBHHH", source_port, destination_port, sequence, acknowledgment,
        offset, flags, window, 0, 0,
    ) + options + payload
    pseudo = struct.pack("!4s4sBBH", source_ip, destination_ip, 0, 6, len(segment))
    segment = segment[:16] + struct.pack("!H", checksum(pseudo + segment)) + segment[18:]
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(segment),
                     0, 0x4000, 64, 6, 0, source_ip, destination_ip)
    ip = ip[:10] + struct.pack("!H", checksum(ip)) + ip[12:]
    return ethernet(destination_mac, source_mac, 0x0800, ip + segment)


def expect_frame(peer: socket.socket, expected: bytes, label: str) -> None:
    actual = recv_frame(peer)
    require(actual == expected,
            f"{label}: expected {expected.hex()}, got {actual.hex()}")


def expect_silent(peer: socket.socket, mode: str) -> None:
    peer.settimeout(0.2)
    try:
        data = peer.recv(1)
    except socket.timeout:
        return
    finally:
        peer.settimeout(TIMEOUT)
    raise RuntimeError(f"expected a quiet packet socket after {mode}, got {data!r}")


def run_probe(vm: VirtualMachine, peer: socket.socket, mode: str,
              *, retry_syn: bool = False) -> None:
    start = vm.send_command(f"{APP} {mode}")
    if mode != "entropy":
        routed = mode == "routed"
        expect_frame(peer, arp_frame(1, b"\xff" * 6, GUEST_MAC, GUEST_IP,
                                     b"\x00" * 6, PEER_IP), "guest ARP request")
        send_frame(peer, arp_frame(2, GUEST_MAC, PEER_MAC, PEER_IP,
                                   GUEST_MAC, GUEST_IP))
        syn = recv_frame(peer)
        local_port, sequence = struct.unpack_from("!H2xI", syn, 34)
        require(49152 <= local_port <= 65535,
                f"SYN used a non-ephemeral port {local_port}")
        expected_syn = tcp_frame(local_port, PEER_PORT, sequence, 0, SYN,
                                 options=GUEST_MSS, routed=routed, from_guest=True)
        require(syn == expected_syn, f"guest SYN differs from expected: {syn.hex()}")
        if retry_syn:
            expect_frame(peer, expected_syn, "retransmitted SYN")
        next_sequence = (sequence + 1) & 0xFFFFFFFF
        if mode == "refuse":
            send_frame(peer, tcp_frame(PEER_PORT, local_port, 0, next_sequence,
                                       RST | ACK))
        else:
            syn_ack = tcp_frame(PEER_PORT, local_port, PEER_SEQUENCE, next_sequence,
                                SYN | ACK, 4096, PEER_MSS, routed=routed)
            expected_ack = tcp_frame(local_port, PEER_PORT, next_sequence, 0, ACK,
                                     routed=routed, from_guest=True)
            send_frame(peer, syn_ack)
            expect_frame(peer, expected_ack, "final ACK")
            send_frame(peer, syn_ack)
            expect_frame(peer, expected_ack, "repeated final ACK")
            send_frame(peer, tcp_frame(PEER_PORT, local_port, 0, next_sequence,
                                       ACK, 4096, payload=b"p", routed=routed))
            expect_frame(peer, expected_ack, "zero-window probe ACK")
    segment = vm.wait_for(PROMPT, start, TIMEOUT)
    require(f"E3 TCP {mode}: PASS" in segment,
            f"TCP guest omitted {mode} PASS:\n{segment}")
    expect_silent(peer, mode)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--qemu", default="qemu-system-i386")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    source = args.image.resolve()
    if not source.is_file():
        parser.error("--image must name an existing image")
    with tempfile.TemporaryDirectory(prefix="mini-os-network-e3-") as directory:
        temp = Path(directory)
        for mode, cpu in (("open", "max,rdrand=on"), ("entropy", "qemu32,rdrand=off")):
            image = temp / f"e3-{mode}.img"
            log = temp / f"e3-{mode}.log"
            capture = temp / f"e3-{mode}.pcap"
            port = reserve_port()
            vm = VirtualMachine(
                image, log, args.qemu, 220 if mode == "open" else 221,
                extra_args=network_args(port, capture, cpu=cpu),
            )
            try:
                try:
                    prepare_debug_image(repo, source, image)
                    vm.start()
                    with connect_peer(port) as peer:
                        peer.settimeout(TIMEOUT)
                        run_probe(vm, peer, mode, retry_syn=(mode == "open"))
                        run_probe(vm, peer, mode)
                        if mode == "open":
                            run_probe(vm, peer, "routed")
                            run_probe(vm, peer, "refuse")
                    vm.command_expect("pwd", PROMPT)
                finally:
                    vm.stop()
                check_image(repo / "build/check_image", image, repo)
            except Exception:
                available = [path for path in (log, capture) if path.is_file()]
                if available:
                    root = repo / "build/test-artifacts"
                    root.mkdir(parents=True, exist_ok=True)
                    destination = Path(tempfile.mkdtemp(prefix="e3-failure-", dir=root))
                    for path in available:
                        shutil.copy2(path, destination / path.name)
                    print(f"E3 failure artifacts retained in {destination}", file=sys.stderr)
                raise
    print("E3 QEMU active open, routing, zero-window ACK, refusal, entropy and guards: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"E3 guest contract: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
