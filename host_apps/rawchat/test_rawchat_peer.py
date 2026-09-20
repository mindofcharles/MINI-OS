#!/usr/bin/env python3
"""Deterministic tests for the Raw Ethernet Chat host application."""

from __future__ import annotations

import importlib.util
from pathlib import Path
from collections import deque
import socket
import struct
import sys
import threading
import time
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
MODULE_PATH = REPOSITORY / "host_apps/rawchat/rawchat_peer.py"
SPEC = importlib.util.spec_from_file_location("rawchat_peer", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("unable to load rawchat_peer.py")
rawchat = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = rawchat
SPEC.loader.exec_module(rawchat)


class ProtocolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mini_mac = rawchat.DEFAULT_MINI_MAC
        self.peer_mac = rawchat.DEFAULT_PEER_MAC

    def text_frame(self, sequence: int = 1, payload: bytes = b"hello") -> bytes:
        return rawchat.build_frame(
            self.mini_mac,
            self.peer_mac,
            rawchat.Message(rawchat.MESSAGE_TEXT, 0x11223344, sequence, payload),
        )

    def test_known_wire_layout_and_padding(self) -> None:
        frame = self.text_frame(sequence=0x55667788)
        self.assertEqual(len(frame), rawchat.FRAME_MIN)
        self.assertEqual(frame[0:6], self.peer_mac)
        self.assertEqual(frame[6:12], self.mini_mac)
        self.assertEqual(frame[12:14], b"\x88\xb5")
        self.assertEqual(frame[14:18], b"\x01\x01\x02\x00")
        self.assertEqual(frame[18:22], b"\x11\x22\x33\x44")
        self.assertEqual(frame[22:26], b"\x55\x66\x77\x88")
        self.assertEqual(frame[26:28], b"\x00\x05")
        self.assertEqual(frame[28:33], b"hello")
        self.assertEqual(frame[33:], bytes(rawchat.FRAME_MIN - 33))

        parsed = rawchat.parse_frame(frame, self.peer_mac, self.mini_mac)
        self.assertEqual(
            parsed,
            rawchat.Message(
                rawchat.MESSAGE_TEXT,
                0x11223344,
                0x55667788,
                b"hello",
            ),
        )

    def test_maximum_payload_and_padding_are_independent(self) -> None:
        payload = bytes(ord("A") + index % 26 for index in range(64))
        frame = self.text_frame(payload=payload)
        self.assertEqual(len(frame), rawchat.WIRE_HEADER_SIZE + rawchat.TEXT_MAX)
        parsed = rawchat.parse_frame(frame, self.peer_mac, self.mini_mac)
        self.assertEqual(parsed.payload, payload)

        padded = bytearray(self.text_frame(payload=b"x"))
        padded[-1] = 0xA5
        parsed = rawchat.parse_frame(bytes(padded), self.peer_mac, self.mini_mac)
        self.assertEqual(parsed.payload, b"x")

    def test_invalid_and_unrelated_frames_are_rejected(self) -> None:
        frame = bytearray(self.text_frame())
        frame[0] ^= 1
        with self.assertRaises(rawchat.UnrelatedFrame):
            rawchat.parse_frame(bytes(frame), self.peer_mac, self.mini_mac)

        cases = []
        frame = bytearray(self.text_frame())
        frame[15] += 1
        cases.append(frame)
        frame = bytearray(self.text_frame())
        frame[17] = 1
        cases.append(frame)
        frame = bytearray(self.text_frame())
        frame[18:22] = bytes(4)
        cases.append(frame)
        frame = bytearray(self.text_frame())
        frame[22:26] = bytes(4)
        cases.append(frame)
        frame = bytearray(self.text_frame())
        frame[26:28] = struct.pack("!H", rawchat.TEXT_MAX)
        cases.append(frame)
        frame = bytearray(self.text_frame())
        frame[28] = 10
        cases.append(frame)

        for invalid in cases:
            with self.subTest(frame=bytes(invalid[14:29])):
                with self.assertRaises(rawchat.RawChatError):
                    rawchat.parse_frame(
                        bytes(invalid), self.peer_mac, self.mini_mac
                    )

    def test_message_builder_validation(self) -> None:
        invalid_messages = (
            rawchat.Message(rawchat.MESSAGE_HELLO, 1, 1, b""),
            rawchat.Message(rawchat.MESSAGE_BYE, 1, 0, b""),
            rawchat.Message(rawchat.MESSAGE_TEXT, 0, 1, b"x"),
            rawchat.Message(rawchat.MESSAGE_TEXT, 1, 1, b""),
            rawchat.Message(rawchat.MESSAGE_TEXT, 1, 1, b"\n"),
            rawchat.Message(rawchat.MESSAGE_TEXT, 1, 1, b"x" * 65),
        )
        for message in invalid_messages:
            with self.subTest(message=message):
                with self.assertRaises(rawchat.RawChatError):
                    rawchat.build_frame(self.mini_mac, self.peer_mac, message)

    def test_peer_session_duplicate_gap_and_bye(self) -> None:
        state = rawchat.PeerState()
        text = rawchat.Message(rawchat.MESSAGE_TEXT, 10, 1, b"one")
        self.assertEqual(state.accept(text), rawchat.PeerResult.NO_SESSION)

        hello = rawchat.Message(rawchat.MESSAGE_HELLO, 10, 0, b"")
        self.assertEqual(state.accept(hello), rawchat.PeerResult.STARTED)
        self.assertEqual(state.accept(hello), rawchat.PeerResult.DUPLICATE)
        self.assertEqual(state.accept(text), rawchat.PeerResult.ACCEPTED)
        self.assertEqual(state.accept(text), rawchat.PeerResult.DUPLICATE)
        self.assertEqual(
            state.accept(rawchat.Message(rawchat.MESSAGE_TEXT, 10, 3, b"three")),
            rawchat.PeerResult.GAP,
        )
        self.assertEqual(
            state.accept(rawchat.Message(rawchat.MESSAGE_BYE, 10, 4, b"")),
            rawchat.PeerResult.ACCEPTED,
        )
        self.assertFalse(state.active)
        self.assertEqual(
            state.accept(rawchat.Message(rawchat.MESSAGE_TEXT, 10, 5, b"late")),
            rawchat.PeerResult.NO_SESSION,
        )


class StreamDecoderTests(unittest.TestCase):
    def frame(self, sequence: int, text: bytes) -> bytes:
        return rawchat.build_frame(
            rawchat.DEFAULT_MINI_MAC,
            rawchat.DEFAULT_PEER_MAC,
            rawchat.Message(rawchat.MESSAGE_TEXT, 7, sequence, text),
        )

    def test_fragmented_prefix_and_frame(self) -> None:
        frame = self.frame(1, b"fragmented")
        encoded = rawchat.encode_stream_frame(frame)
        decoder = rawchat.FrameDecoder()
        output = []
        for byte in encoded:
            output.extend(decoder.feed(bytes([byte])))
        self.assertEqual(output, [frame])

    def test_coalesced_consecutive_frames(self) -> None:
        first = self.frame(1, b"first")
        second = self.frame(2, b"second")
        decoder = rawchat.FrameDecoder()
        self.assertEqual(
            decoder.feed(
                rawchat.encode_stream_frame(first)
                + rawchat.encode_stream_frame(second)
            ),
            [first, second],
        )

    def test_invalid_stream_lengths(self) -> None:
        for length in (0, rawchat.FRAME_MIN - 1, rawchat.FRAME_MAX + 1):
            with self.subTest(length=length):
                decoder = rawchat.FrameDecoder()
                with self.assertRaises(rawchat.StreamFramingError):
                    decoder.feed(struct.pack("!I", length))


class FakeKeyboard:
    def __init__(self) -> None:
        self._keys = deque()
        self._lock = threading.Lock()

    def extend(self, text: str) -> None:
        with self._lock:
            self._keys.extend(text)

    def poll(self):
        with self._lock:
            if not self._keys:
                return None
            return self._keys.popleft()


class FakeUI:
    def __init__(self) -> None:
        self.input_text = ""
        self.status_text = ""
        self.events = []
        self._lock = threading.Lock()

    def redraw(self) -> None:
        pass

    def event(self, text: str) -> None:
        with self._lock:
            self.events.append(text)

    def status(self, text: str) -> None:
        self.status_text = text
        self.event("[status] " + text)

    def contains(self, text: str) -> bool:
        with self._lock:
            return any(text in event for event in self.events)


class ConnectionLoopTests(unittest.TestCase):
    def receive_messages(
        self,
        connection: socket.socket,
        decoder,
        message_type: int,
        count: int,
    ):
        messages = []
        deadline = time.monotonic() + 2.0
        while len(messages) < count:
            remaining = deadline - time.monotonic()
            self.assertGreater(remaining, 0, "timed out receiving peer messages")
            connection.settimeout(remaining)
            data = connection.recv(4096)
            self.assertTrue(data, "connection loop closed unexpectedly")
            for frame in decoder.feed(data):
                message = rawchat.parse_frame(
                    frame,
                    rawchat.DEFAULT_MINI_MAC,
                    rawchat.DEFAULT_PEER_MAC,
                )
                if message.message_type == message_type:
                    messages.append(message)
        return messages

    def wait_for_event(self, ui: FakeUI, expected: str) -> None:
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            if ui.contains(expected):
                return
            time.sleep(0.005)
        self.fail("timed out waiting for UI event: " + expected)

    def test_full_duplex_connection_loop(self) -> None:
        application, peer = socket.socketpair()
        keyboard = FakeKeyboard()
        ui = FakeUI()
        state = rawchat.PeerState()
        result = []
        failure = []

        def run_loop() -> None:
            try:
                result.append(
                    rawchat.run_connection(
                        application,
                        ("local", 1),
                        keyboard,
                        ui,
                        rawchat.DEFAULT_MINI_MAC,
                        rawchat.DEFAULT_PEER_MAC,
                        state,
                    )
                )
            except BaseException as error:
                failure.append(error)

        worker = threading.Thread(target=run_loop)
        worker.start()
        decoder = rawchat.FrameDecoder()
        try:
            self.receive_messages(
                peer, decoder, rawchat.MESSAGE_HELLO, 1
            )
            peer_session = 0x10203040
            inbound = (
                rawchat.encode_stream_frame(
                    rawchat.build_frame(
                        rawchat.DEFAULT_MINI_MAC,
                        rawchat.DEFAULT_PEER_MAC,
                        rawchat.Message(
                            rawchat.MESSAGE_HELLO, peer_session, 0, b""
                        ),
                    )
                )
                + rawchat.encode_stream_frame(
                    rawchat.build_frame(
                        rawchat.DEFAULT_MINI_MAC,
                        rawchat.DEFAULT_PEER_MAC,
                        rawchat.Message(
                            rawchat.MESSAGE_TEXT, peer_session, 1, b"first"
                        ),
                    )
                )
                + rawchat.encode_stream_frame(
                    rawchat.build_frame(
                        rawchat.DEFAULT_MINI_MAC,
                        rawchat.DEFAULT_PEER_MAC,
                        rawchat.Message(
                            rawchat.MESSAGE_TEXT, peer_session, 2, b"second"
                        ),
                    )
                )
            )
            peer.sendall(inbound)
            self.wait_for_event(ui, "MINI-OS: first")
            self.wait_for_event(ui, "MINI-OS: second")

            keyboard.extend("one\rtwo\r")
            outbound = self.receive_messages(
                peer, decoder, rawchat.MESSAGE_TEXT, 2
            )
            self.assertEqual(
                [message.payload for message in outbound], [b"one", b"two"]
            )
            self.assertEqual(
                [message.sequence for message in outbound], [1, 2]
            )

            keyboard.extend("\x1b")
            bye = self.receive_messages(
                peer, decoder, rawchat.MESSAGE_BYE, 1
            )[0]
            self.assertEqual(bye.sequence, 3)
            worker.join(2.0)
            self.assertFalse(worker.is_alive(), "connection loop did not exit")
            self.assertFalse(failure, f"connection loop failed: {failure}")
            self.assertEqual(result, [True])
        finally:
            keyboard.extend("\x1b")
            application.close()
            peer.close()
            worker.join(2.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
