#!/usr/bin/env python3
"""Exercise TCP active open, byte streams and loss recovery with a QEMU peer."""

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
SYN, RST, PSH, ACK = 0x02, 0x04, 0x08, 0x10
GUEST_MSS = bytes.fromhex("02 04 02 18")
PEER_MSS = bytes.fromhex("02 04 05 b4")
STREAM_MSS = bytes.fromhex("02 04 01 37")
QUEUE_CAPACITY = 4096
STREAM_TX_COUNT, STREAM_RX_COUNT = 7351, 7129
RETRY_BYTES = bytes.fromhex("45 00 35 61 ff 0d 00 80 42")
RECOVERY_MSS = 128
RECOVERY_WINDOW = 5 * RECOVERY_MSS
RECOVERY_TX_COUNT = 16 * RECOVERY_MSS
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


def stream_bytes(offset: int, length: int, salt: int) -> bytes:
    return bytes((index * 29 + index // 251 + salt) & 0xFF
                 for index in range(offset, offset + length))


def exchange_stream(peer: socket.socket, local_port: int, first_sequence: int) -> None:
    received = supplied = acknowledged = last_ack = 0
    right = QUEUE_CAPACITY

    for index in range(512):
        size = min(right - supplied, STREAM_RX_COUNT - supplied, 193 if index % 2 else 277)
        if size or last_ack != received:
            send_frame(peer, tcp_frame(
                PEER_PORT, local_port, supplied, (first_sequence + received) & 0xFFFFFFFF,
                ACK, 400, payload=stream_bytes(supplied, size, 77), routed=True,
            ))
            supplied += size
            last_ack = received
        if (received == STREAM_TX_COUNT and acknowledged == STREAM_RX_COUNT and
                right == STREAM_RX_COUNT + QUEUE_CAPACITY):
            return

        actual = recv_frame(peer)
        length = struct.unpack_from("!H", actual, 16)[0] - 40
        next_ack, offset, flags, window = struct.unpack_from("!IBBH", actual, 42)
        require(acknowledged <= next_ack <= supplied, "guest ACK is outside the peer's stream")
        require(right <= next_ack + window <= next_ack + QUEUE_CAPACITY,
                "guest withdrew receive credit or exceeded its queue")
        require(0 <= length <= 311 and length + received <= STREAM_TX_COUNT,
                "guest exceeded its effective MSS or the stream length")
        require(offset == 0x50 and
                (flags == ACK or (length != 0 and flags == (ACK | PSH))),
                "unexpected TCP stream options or flags")
        expected = tcp_frame(
            local_port, PEER_PORT, (first_sequence + received) & 0xFFFFFFFF,
            next_ack, flags, window, payload=stream_bytes(received, length, 0),
            routed=True, from_guest=True,
        )
        require(actual == expected, f"guest stream differs at byte {received}")
        received += length
        acknowledged = next_ack
        right = next_ack + window
    raise RuntimeError("E4 streams did not complete within the peer frame bound")


def recover_dropped_data(peer: socket.socket, local_port: int, first_sequence: int) -> None:
    expected = tcp_frame(local_port, PEER_PORT, first_sequence, 0, ACK | PSH,
                         QUEUE_CAPACITY, payload=RETRY_BYTES, routed=True, from_guest=True)
    expect_frame(peer, expected, "first data transmission")
    # Discard the first copy: the peer advances its ACK only after seeing the retry.
    expect_frame(peer, expected, "retransmitted data at the same sequence")
    acknowledgment = (first_sequence + len(RETRY_BYTES)) & 0xFFFFFFFF
    send_frame(peer, tcp_frame(PEER_PORT, local_port, 0, acknowledgment,
                               ACK, QUEUE_CAPACITY, routed=True))


def recover_two_losses(peer: socket.socket, local_port: int, first_sequence: int) -> None:
    def data(offset: int, label: str) -> None:
        flags = ACK | PSH if offset + RECOVERY_MSS == RECOVERY_TX_COUNT else ACK
        expect_frame(peer, tcp_frame(
            local_port, PEER_PORT, (first_sequence + offset) & 0xFFFFFFFF, 0,
            flags, QUEUE_CAPACITY, payload=stream_bytes(offset, RECOVERY_MSS, 45),
            routed=True, from_guest=True,
        ), label)

    def acknowledge(offset: int) -> None:
        send_frame(peer, tcp_frame(
            PEER_PORT, local_port, 0, (first_sequence + offset) & 0xFFFFFFFF,
            ACK, RECOVERY_WINDOW, routed=True,
        ))

    offset = 0
    for flight in range(1, 5):
        for _ in range(flight):
            data(offset, f"slow-start flight of {flight} before an ACK")
            offset += RECOVERY_MSS
        acknowledge(offset)

    first_loss = offset
    for segment in range(5):
        data(offset, f"five-segment loss flight, segment {segment + 1}")
        offset += RECOVERY_MSS
    # Drop A and C; buffering B, D and E produces three duplicate ACKs for A.
    for _ in range(3):
        acknowledge(first_loss)
    data(first_loss, "fast retransmission of A")
    second_loss = first_loss + 2 * RECOVERY_MSS
    acknowledge(second_loss)
    data(second_loss, "repair of C after a partial ACK")
    # An RTO would reduce cwnd below the three segments still outstanding here.
    # Receiving new F before another cumulative ACK proves windowed recovery.
    data(offset, "new data during fast recovery")
    acknowledge(offset + RECOVERY_MSS)


def run_probe(vm: VirtualMachine, peer: socket.socket, mode: str,
              *, retry_syn: bool = False) -> None:
    start = vm.send_command(f"{APP} {mode}")
    if mode != "entropy":
        routed = mode in ("routed", "stream", "retransmit", "recovery")
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
            stream = mode == "stream"
            window = 400 if stream else QUEUE_CAPACITY
            options = STREAM_MSS if stream else PEER_MSS
            if mode == "recovery":
                window = RECOVERY_WINDOW
                options = struct.pack("!BBH", 2, 4, RECOVERY_MSS)
            syn_ack = tcp_frame(PEER_PORT, local_port, PEER_SEQUENCE, next_sequence,
                                SYN | ACK, window, options, routed=routed)
            expected_ack = tcp_frame(local_port, PEER_PORT, next_sequence, 0, ACK,
                                     QUEUE_CAPACITY, routed=routed, from_guest=True)
            send_frame(peer, syn_ack)
            expect_frame(peer, expected_ack, "final ACK")
            if stream:
                exchange_stream(peer, local_port, next_sequence)
            elif mode == "retransmit":
                recover_dropped_data(peer, local_port, next_sequence)
            elif mode == "recovery":
                recover_two_losses(peer, local_port, next_sequence)
            else:
                send_frame(peer, syn_ack)
                expect_frame(peer, expected_ack, "repeated final ACK")
    segment = vm.wait_for(PROMPT, start, TIMEOUT)
    marker = {
        "stream": "E4 TCP stream: PASS",
        "retransmit": "E5a TCP retransmit: PASS",
        "recovery": "E5b TCP fast recovery: PASS",
    }.get(mode, f"E3 TCP {mode}: PASS")
    require(marker in segment,
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
    with tempfile.TemporaryDirectory(prefix="mini-os-network-tcp-") as directory:
        temp = Path(directory)
        for mode, cpu in (("open", "max,rdrand=on"), ("entropy", "qemu32,rdrand=off")):
            image = temp / f"tcp-{mode}.img"
            log = temp / f"tcp-{mode}.log"
            capture = temp / f"tcp-{mode}.pcap"
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
                            run_probe(vm, peer, "stream")
                            run_probe(vm, peer, "retransmit")
                            run_probe(vm, peer, "recovery")
                    vm.command_expect("pwd", PROMPT)
                finally:
                    vm.stop()
                check_image(repo / "build/check_image", image, repo)
            except Exception:
                available = [path for path in (log, capture) if path.is_file()]
                if available:
                    root = repo / "build/test-artifacts"
                    root.mkdir(parents=True, exist_ok=True)
                    destination = Path(tempfile.mkdtemp(prefix="tcp-failure-", dir=root))
                    for path in available:
                        shutil.copy2(path, destination / path.name)
                    print(f"TCP failure artifacts retained in {destination}", file=sys.stderr)
                raise
    print("TCP QEMU active open, routed streams, loss recovery and guards: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"TCP guest contract: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
