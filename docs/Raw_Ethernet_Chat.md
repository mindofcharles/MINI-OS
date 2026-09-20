# Raw Ethernet Chat

> [!NOTE]
> I wrote the initial version of the "Raw Ethernet Chat" application (consisting of just two files and a total of 300 lines of code), and then GPT extensively refined it based on my requirements. (GPT really loves writing tests.)

Raw Ethernet Chat is a small full-duplex demonstration built directly on the MINI-OS raw-frame interface.

It uses the local experimental EtherType `0x88B5` and does not use IPv4 or TCP.

## Build

```bash
make app APP=rawchat
```

All application sources compile as strict C90.

Its private frame encoder, parser, and peer-session checks live in `transport/apps/rawchat/protocol.c` beside `main.c`; they are not part of the general network library.

## Run on one computer

Start the host application first:

```bash
python3 host_apps/rawchat/rawchat_peer.py
```

Start QEMU in another terminal:

```bash
make run-rawchat
```

The QEMU target retries the stream connection, so the two processes do not require a strict start order.

Run the application from the MINI-OS shell:

```text
run /transport/build/apps/rawchat.bin
```

Both sides may send several messages without waiting for a reply.

- Type up to 64 printable ASCII characters.

- Press `Enter` to send the current line.

- Press `Backspace` to edit the current line.

- Press `Esc` to leave the chat.

## Run across two computers

> [!CAUTION]
> This is an unauthenticated and unencrypted development tool, so a non-loopback listener must be used only on a trusted network.

The host application listens only on loopback by default.

To accept a trusted LAN connection on the host computer, run:

```bash
python3 host_apps/rawchat/rawchat_peer.py --bind 0.0.0.0
```

Then start QEMU with the host computer's address:

```bash
make run-rawchat RAWCHAT_HOST=192.168.1.50
```

Replace `192.168.1.50` with the actual host address and allow the selected TCP port through the host firewall when required.

## Protocol and safety

QEMU transports complete Ethernet packets over a TCP stream with a four-byte network-order length prefix.

Raw Chat v1 adds a subtype, protocol version, message type, session identifier, sequence number, and explicit payload length before each text payload.

The receiver validates the endpoint MAC addresses and all protocol fields, ignores Ethernet padding, rejects control characters, suppresses duplicates, and reports sequence gaps.
