"""Shared framing and connection helpers for QEMU's TCP packet socket."""

from __future__ import annotations

from pathlib import Path
import socket
import struct
import time
from typing import Optional


FRAME_MIN = 60
FRAME_MAX = 1514


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


def network_args(port: int, capture: Path, io_base: int = 0x300) -> list[str]:
    return [
        "-accel",
        "tcg",
        "-cpu",
        "max,rdrand=on",
        "-netdev",
        f"socket,id=net0,listen=127.0.0.1:{port}",
        "-device",
        f"ne2k_isa,netdev=net0,iobase={hex(io_base)},irq=9,"
        "mac=52:54:00:12:34:56",
        "-object",
        f"filter-dump,id=netdump,netdev=net0,file={capture}",
    ]
