#!/usr/bin/env python3
"""Full-duplex host application for the MINI-OS Raw Chat v1 protocol."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from enum import Enum, auto
import os
import secrets
import select
import socket
import struct
import sys
import time
from typing import Optional


FRAME_MIN = 60
FRAME_MAX = 1514
ETHERNET_HEADER_SIZE = 14
PROTOCOL_HEADER_SIZE = 14
WIRE_HEADER_SIZE = ETHERNET_HEADER_SIZE + PROTOCOL_HEADER_SIZE
TEXT_MAX = 64

ETHERTYPE = 0x88B5
SUBTYPE = 1
VERSION = 1
MESSAGE_HELLO = 1
MESSAGE_TEXT = 2
MESSAGE_BYE = 3

DEFAULT_PORT = 12345
DEFAULT_MINI_MAC = bytes.fromhex("52 54 00 12 34 56")
DEFAULT_PEER_MAC = bytes.fromhex("02 00 00 00 00 01")


class RawChatError(ValueError):
    """A frame targets Raw Chat but violates its wire contract."""


class UnrelatedFrame(ValueError):
    """An Ethernet frame does not belong to this Raw Chat endpoint."""


class StreamFramingError(ValueError):
    """The QEMU stream has an invalid packet-length prefix."""


class PeerResult(Enum):
    ACCEPTED = auto()
    GAP = auto()
    STARTED = auto()
    DUPLICATE = auto()
    NO_SESSION = auto()


@dataclass(frozen=True)
class Message:
    message_type: int
    session_id: int
    sequence: int
    payload: bytes


@dataclass
class PeerState:
    session_id: int = 0
    last_sequence: int = 0
    active: bool = False

    def accept(self, message: Message) -> PeerResult:
        validate_message(message)
        if message.message_type == MESSAGE_HELLO:
            if self.active and self.session_id == message.session_id:
                return PeerResult.DUPLICATE
            self.session_id = message.session_id
            self.last_sequence = 0
            self.active = True
            return PeerResult.STARTED

        if not self.active or self.session_id != message.session_id:
            return PeerResult.NO_SESSION
        if message.sequence <= self.last_sequence:
            return PeerResult.DUPLICATE

        gap = message.sequence != self.last_sequence + 1
        self.last_sequence = message.sequence
        if message.message_type == MESSAGE_BYE:
            self.active = False
        return PeerResult.GAP if gap else PeerResult.ACCEPTED


class FrameDecoder:
    """Incrementally decode QEMU's four-byte-length packet stream."""

    def __init__(self) -> None:
        self._buffer = bytearray()
        self._expected_length: Optional[int] = None

    def feed(self, data: bytes) -> list[bytes]:
        self._buffer.extend(data)
        frames: list[bytes] = []
        while True:
            if self._expected_length is None:
                if len(self._buffer) < 4:
                    break
                self._expected_length = struct.unpack("!I", self._buffer[:4])[0]
                del self._buffer[:4]
                if not FRAME_MIN <= self._expected_length <= FRAME_MAX:
                    raise StreamFramingError(
                        f"invalid Ethernet frame length: {self._expected_length}"
                    )
            if len(self._buffer) < self._expected_length:
                break
            frames.append(bytes(self._buffer[: self._expected_length]))
            del self._buffer[: self._expected_length]
            self._expected_length = None
        return frames


def parse_mac(text: str) -> bytes:
    parts = text.split(":")
    if len(parts) != 6 or any(len(part) != 2 for part in parts):
        raise argparse.ArgumentTypeError("MAC addresses require six hex bytes")
    try:
        mac = bytes(int(part, 16) for part in parts)
    except ValueError as error:
        raise argparse.ArgumentTypeError("MAC address contains invalid hex") from error
    try:
        validate_mac(mac)
    except RawChatError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    return mac


def validate_mac(mac: bytes) -> None:
    if len(mac) != 6:
        raise RawChatError("MAC address must contain six bytes")
    if not any(mac) or mac[0] & 1:
        raise RawChatError("Raw Chat requires a nonzero unicast MAC address")


def validate_message(message: Message) -> None:
    if not 0 < message.session_id <= 0xFFFFFFFF:
        raise RawChatError("session id must be a nonzero 32-bit value")
    if not 0 <= message.sequence <= 0xFFFFFFFF:
        raise RawChatError("sequence must be a 32-bit value")
    if message.message_type == MESSAGE_HELLO:
        if message.sequence != 0 or message.payload:
            raise RawChatError("HELLO requires sequence zero and no payload")
        return
    if message.message_type == MESSAGE_BYE:
        if message.sequence == 0 or message.payload:
            raise RawChatError("BYE requires a nonzero sequence and no payload")
        return
    if message.message_type != MESSAGE_TEXT:
        raise RawChatError("unknown Raw Chat message type")
    if message.sequence == 0 or not 1 <= len(message.payload) <= TEXT_MAX:
        raise RawChatError("TEXT requires a sequence and 1 to 64 bytes")
    if any(value < 32 or value > 126 for value in message.payload):
        raise RawChatError("TEXT payload must be printable ASCII")


def build_frame(source_mac: bytes, destination_mac: bytes, message: Message) -> bytes:
    validate_mac(source_mac)
    validate_mac(destination_mac)
    if source_mac == destination_mac:
        raise RawChatError("source and destination MAC addresses must differ")
    validate_message(message)

    protocol = struct.pack(
        "!BBBBIIH",
        SUBTYPE,
        VERSION,
        message.message_type,
        0,
        message.session_id,
        message.sequence,
        len(message.payload),
    )
    frame = bytearray(
        destination_mac
        + source_mac
        + struct.pack("!H", ETHERTYPE)
        + protocol
        + message.payload
    )
    if len(frame) < FRAME_MIN:
        frame.extend(b"\0" * (FRAME_MIN - len(frame)))
    return bytes(frame)


def parse_frame(frame: bytes, local_mac: bytes, peer_mac: bytes) -> Message:
    validate_mac(local_mac)
    validate_mac(peer_mac)
    if not FRAME_MIN <= len(frame) <= FRAME_MAX:
        raise RawChatError("Ethernet frame length is outside the raw ABI")
    if (
        frame[0:6] != local_mac
        or frame[6:12] != peer_mac
        or struct.unpack("!H", frame[12:14])[0] != ETHERTYPE
        or frame[14] != SUBTYPE
    ):
        raise UnrelatedFrame("frame does not belong to this Raw Chat endpoint")
    if frame[15] != VERSION or frame[17] != 0:
        raise RawChatError("unsupported version or nonzero flags")

    message_type = frame[16]
    session_id, sequence, payload_length = struct.unpack("!IIH", frame[18:28])
    if payload_length > TEXT_MAX or WIRE_HEADER_SIZE + payload_length > len(frame):
        raise RawChatError("declared payload length is invalid")
    message = Message(
        message_type,
        session_id,
        sequence,
        bytes(frame[WIRE_HEADER_SIZE : WIRE_HEADER_SIZE + payload_length]),
    )
    validate_message(message)
    return message


def encode_stream_frame(frame: bytes) -> bytes:
    if not FRAME_MIN <= len(frame) <= FRAME_MAX:
        raise StreamFramingError("Ethernet frame length is outside the raw ABI")
    return struct.pack("!I", len(frame)) + frame


def new_session_id() -> int:
    value = 0
    while value == 0:
        value = secrets.randbits(32)
    return value


class Keyboard:
    def __init__(self) -> None:
        self._saved_attributes: Optional[object] = None
        self._fd: Optional[int] = None
        self._windows_module: Optional[object] = None
        self._discard_windows_extended = False

    def __enter__(self) -> "Keyboard":
        if os.name == "nt":
            import msvcrt

            self._windows_module = msvcrt
            return self

        if not sys.stdin.isatty():
            raise RuntimeError("interactive input requires a terminal")
        import termios
        import tty

        self._fd = sys.stdin.fileno()
        self._saved_attributes = termios.tcgetattr(self._fd)
        tty.setcbreak(self._fd)
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        if self._saved_attributes is not None and self._fd is not None:
            import termios

            termios.tcsetattr(self._fd, termios.TCSADRAIN, self._saved_attributes)

    def poll(self) -> Optional[str]:
        if self._windows_module is not None:
            msvcrt = self._windows_module
            if not msvcrt.kbhit():
                return None
            key = msvcrt.getwch()
            if self._discard_windows_extended:
                self._discard_windows_extended = False
                return None
            if key in ("\x00", "\xe0"):
                if msvcrt.kbhit():
                    msvcrt.getwch()
                else:
                    self._discard_windows_extended = True
                return None
            return key

        if self._fd is None:
            return None
        readable, _, _ = select.select([self._fd], [], [], 0)
        if not readable:
            return None
        return os.read(self._fd, 1).decode("latin1")


class TerminalUI:
    def __init__(self) -> None:
        self.input_text = ""
        self.status_text = ""
        self._prompt_width = 0

    def _clear_prompt(self) -> None:
        sys.stdout.write("\r" + " " * self._prompt_width + "\r")
        self._prompt_width = 0

    def redraw(self) -> None:
        self._clear_prompt()
        prompt = f"You> {self.input_text}"
        sys.stdout.write(prompt)
        self._prompt_width = len(prompt)
        sys.stdout.flush()

    def event(self, text: str) -> None:
        self._clear_prompt()
        sys.stdout.write(text + "\n")
        self.redraw()

    def status(self, text: str) -> None:
        if text != self.status_text:
            self.status_text = text
            self.event(f"[status] {text}")

    def close_prompt(self) -> None:
        self._clear_prompt()
        sys.stdout.flush()


def queue_message(
    outgoing: bytearray,
    source_mac: bytes,
    destination_mac: bytes,
    message: Message,
) -> None:
    outgoing.extend(
        encode_stream_frame(build_frame(source_mac, destination_mac, message))
    )


def flush_before_close(connection: socket.socket, outgoing: bytearray) -> None:
    deadline = time.monotonic() + 0.25
    while outgoing and time.monotonic() < deadline:
        _, writable, _ = select.select([], [connection], [], 0.05)
        if not writable:
            continue
        try:
            sent = connection.send(outgoing)
        except (BlockingIOError, InterruptedError):
            continue
        except (ConnectionError, OSError):
            return
        if sent <= 0:
            return
        del outgoing[:sent]


def run_connection(
    connection: socket.socket,
    address: tuple[object, ...],
    keyboard: Keyboard,
    ui: TerminalUI,
    mini_mac: bytes,
    peer_mac: bytes,
    peer_state: PeerState,
) -> bool:
    connection.setblocking(False)
    decoder = FrameDecoder()
    outgoing = bytearray()
    local_session = new_session_id()
    local_sequence = 0

    def send_hello() -> None:
        queue_message(
            outgoing,
            peer_mac,
            mini_mac,
            Message(MESSAGE_HELLO, local_session, 0, b""),
        )

    send_hello()
    ui.status(f"QEMU connected from {address[0]}:{address[1]}")

    while True:
        for _ in range(8):
            key = keyboard.poll()
            if key is None:
                break
            if key == "\x1b":
                if local_sequence < 0xFFFFFFFF:
                    local_sequence += 1
                    queue_message(
                        outgoing,
                        peer_mac,
                        mini_mac,
                        Message(MESSAGE_BYE, local_session, local_sequence, b""),
                    )
                flush_before_close(connection, outgoing)
                return True
            if key in ("\b", "\x7f"):
                ui.input_text = ui.input_text[:-1]
                ui.redraw()
                continue
            if key in ("\r", "\n"):
                if not ui.input_text:
                    ui.status("type a message before pressing Enter")
                    continue
                if local_sequence == 0:
                    send_hello()
                if local_sequence == 0xFFFFFFFF:
                    local_session = new_session_id()
                    local_sequence = 0
                    send_hello()
                encoded = ui.input_text.encode("ascii")
                local_sequence += 1
                queue_message(
                    outgoing,
                    peer_mac,
                    mini_mac,
                    Message(
                        MESSAGE_TEXT,
                        local_session,
                        local_sequence,
                        encoded,
                    ),
                )
                sent_text = ui.input_text
                ui.input_text = ""
                ui.event(f"You: {sent_text}")
                continue
            if " " <= key <= "~":
                if len(ui.input_text) >= TEXT_MAX:
                    ui.status("message limit reached: 64 characters")
                else:
                    ui.input_text += key
                    ui.redraw()

        read_list = [connection]
        write_list = [connection] if outgoing else []
        try:
            readable, writable, _ = select.select(
                read_list, write_list, [], 0.05
            )
            if writable:
                try:
                    sent = connection.send(outgoing)
                except (BlockingIOError, InterruptedError):
                    sent = None
                if sent is not None:
                    if sent <= 0:
                        raise ConnectionError("QEMU connection closed during send")
                    del outgoing[:sent]
            if readable:
                try:
                    data = connection.recv(65536)
                except (BlockingIOError, InterruptedError):
                    data = None
                if data == b"":
                    return False
                for frame in decoder.feed(data or b""):
                    try:
                        message = parse_frame(frame, peer_mac, mini_mac)
                    except UnrelatedFrame:
                        continue
                    except RawChatError as error:
                        ui.status(f"ignored malformed Raw Chat frame: {error}")
                        continue

                    result = peer_state.accept(message)
                    if result == PeerResult.DUPLICATE:
                        ui.status("ignored duplicate MINI-OS message")
                        continue
                    if result == PeerResult.NO_SESSION:
                        ui.status("ignored MINI-OS message without a current HELLO")
                        send_hello()
                        continue
                    if result == PeerResult.GAP:
                        ui.event("[warning] one or more MINI-OS messages were lost")

                    if message.message_type == MESSAGE_HELLO:
                        ui.status("MINI-OS session is ready; full-duplex messaging is active")
                        send_hello()
                    elif message.message_type == MESSAGE_TEXT:
                        ui.event("MINI-OS: " + message.payload.decode("ascii"))
                    else:
                        ui.status("MINI-OS session ended")
        except (ConnectionError, OSError, StreamFramingError) as error:
            ui.status(f"QEMU disconnected: {error}")
            return False


def serve(args: argparse.Namespace) -> int:
    ui = TerminalUI()
    peer_state = PeerState()
    with Keyboard() as keyboard, socket.socket(
        socket.AF_INET, socket.SOCK_STREAM
    ) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.bind, args.port))
        server.listen(1)
        server.setblocking(False)
        ui.event(
            f"Raw Ethernet Chat peer listening on {args.bind}:{args.port}; "
            "press ESC to exit."
        )
        if args.bind not in ("127.0.0.1", "localhost"):
            ui.status("the unauthenticated test service is reachable beyond this host")

        while True:
            key = keyboard.poll()
            if key == "\x1b":
                ui.close_prompt()
                return 0
            readable, _, _ = select.select([server], [], [], 0.05)
            if not readable:
                continue
            connection, address = server.accept()
            with connection:
                quit_requested = run_connection(
                    connection,
                    address,
                    keyboard,
                    ui,
                    args.mini_mac,
                    args.peer_mac,
                    peer_state,
                )
            if quit_requested:
                ui.close_prompt()
                return 0
            ui.status("waiting for QEMU to reconnect")


def argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run the host side of MINI-OS Raw Ethernet Chat."
    )
    parser.add_argument(
        "--bind",
        default="127.0.0.1",
        help="listen address (default: 127.0.0.1)",
    )
    parser.add_argument(
        "--port", type=int, default=DEFAULT_PORT, help="TCP port (default: 12345)"
    )
    parser.add_argument(
        "--mini-mac",
        type=parse_mac,
        default=DEFAULT_MINI_MAC,
        help="MINI-OS NE2000 MAC (default: 52:54:00:12:34:56)",
    )
    parser.add_argument(
        "--peer-mac",
        type=parse_mac,
        default=DEFAULT_PEER_MAC,
        help="host peer MAC (default: 02:00:00:00:00:01)",
    )
    return parser


def main() -> int:
    args = argument_parser().parse_args()
    if not 1 <= args.port <= 65535:
        print("error: port must be between 1 and 65535", file=sys.stderr)
        return 2
    if args.mini_mac == args.peer_mac:
        print("error: MINI-OS and peer MAC addresses must differ", file=sys.stderr)
        return 2
    try:
        return serve(args)
    except KeyboardInterrupt:
        sys.stdout.write("\n")
        return 0
    except (OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
