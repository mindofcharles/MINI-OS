"""Shared framing and connection helpers for QEMU's TCP packet socket."""

from __future__ import annotations

from pathlib import Path
import socket
import struct
import time
from typing import Optional


FRAME_MIN = 60
FRAME_MAX = 1514


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
    return frame.ljust(FRAME_MIN, bytes((padding,)))


def arp_frame(operation: int, destination: bytes, source: bytes,
              sender_ip: bytes, target_mac: bytes, target_ip: bytes,
              sender_mac: Optional[bytes] = None) -> bytes:
    sender = source if sender_mac is None else sender_mac
    payload = struct.pack("!HHBBH", 1, 0x0800, 6, 4, operation)
    payload += sender + sender_ip + target_mac + target_ip
    return ethernet(destination, source, 0x0806, payload)


def reserve_port() -> int:
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])
    finally:
        probe.close()


def connect_peer(port: int) -> socket.socket:
    deadline = time.monotonic() + 5.0
    last_error: Optional[OSError] = None
    while time.monotonic() < deadline:
        peer = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        peer.settimeout(5.0)
        try:
            peer.connect(("127.0.0.1", port))
            return peer
        except OSError as error:
            last_error = error
            peer.close()
            time.sleep(0.03)
    raise RuntimeError(f"cannot connect to QEMU packet socket: {last_error}")


def recv_exact(peer: socket.socket, length: int) -> bytes:
    data = bytearray()
    while len(data) < length:
        chunk = peer.recv(length - len(data))
        if not chunk:
            raise RuntimeError("QEMU packet socket closed unexpectedly")
        data.extend(chunk)
    return bytes(data)


def recv_frame(peer: socket.socket) -> bytes:
    length = struct.unpack("!I", recv_exact(peer, 4))[0]
    if length < FRAME_MIN or length > FRAME_MAX:
        raise RuntimeError(f"QEMU emitted invalid Ethernet length {length}")
    return recv_exact(peer, length)


def send_frame(peer: socket.socket, frame: bytes) -> None:
    if len(frame) < FRAME_MIN or len(frame) > FRAME_MAX:
        raise ValueError("test frame is outside normalized bounds")
    peer.sendall(struct.pack("!I", len(frame)) + frame)


def network_args(port: int, capture: Path, io_base: int = 0x300,
                 *, cpu: str = "max,rdrand=on") -> list[str]:
    return [
        "-accel",
        "tcg",
        "-cpu",
        cpu,
        "-netdev",
        f"socket,id=net0,listen=127.0.0.1:{port}",
        "-device",
        f"ne2k_isa,netdev=net0,iobase={hex(io_base)},irq=9,"
        "mac=52:54:00:12:34:56",
        "-object",
        f"filter-dump,id=netdump,netdev=net0,file={capture}",
    ]
