# MINI-OS Ethernet, ARP, IPv4, and ICMP Echo

## Current scope

The network stack is linked into an application and uses the kernel's polling NE2000 raw-frame system calls; it is not a background kernel service.

Protocol parsing, packet construction, timers, and bounded ARP state are modern C under `transport/lib/net/`, while `ping.c` and the application-facing `net.h` interface remain strict-C90 compatible.

The supported application protocol subset is Ethernet II, Ethernet/IPv4 ARP, static-address unfragmented IPv4, and ICMP Echo Request and Reply.

UDP, TCP, DHCP, DNS, SSH, IPv4 options, fragment reassembly, and ICMP error processing are not implemented.

## Configuration and use

`ping.bin` uses address `10.0.2.15`, mask `255.255.255.0`, and gateway `10.0.2.2`, with the NE2000 device at I/O base `0x300`, IRQ 9, and MAC address `52:54:00:12:34:56` in the supported QEMU run target.

From the MINI-OS shell after `make run-network`, run `run /transport/build/apps/ping.bin 10.0.2.2` to send four bounded Echo Requests and print a per-request result plus a summary.

The public `net_init`, `net_poll`, `net_resolve_arp`, and `net_ping` calls use relative deadlines, distinguish timeout from keyboard cancellation and device failure, and allow only one synchronous operation per application context.

The default ARP cache has four entries, a 60-second lifetime, bounded retries, and at most one ARP Request per second for the same application context.

The network context is created afresh for each application run, so ARP entries do not survive application exit.

## Packet behavior and limits

Transmit frames are explicitly zero-padded to the 60-byte Ethernet minimum, while IPv4 total length excludes that padding.

The IPv4 transmitter uses a 20-byte header, a 1,500-byte MTU, TTL 64, and an atomic datagram with DF set and identification zero.

The receiver validates the declared IPv4 and ICMP lengths and checksums, rejects options and fragments, and dispatches only unicast ICMP packets addressed to the configured local address.

An outbound Ping succeeds only for an Echo Reply with the expected addresses, identifier, sequence, payload length, and payload bytes.

The stack responds to a valid unicast Echo Request only while a network application is polling; an idle MINI-OS shell is not continuously pingable.

ARP is unauthenticated, and neither address-conflict defense nor protection against a syntactically valid forged ARP Request is provided.

## Automated validation

`make test-network-host` runs deterministic backend, timing, cache, parser, and packet-construction tests without QEMU.

`make test-network-d6` boots temporary debug images and tests exact ARP and ICMP wire packets through QEMU's NE2000 model against a controlled Ethernet peer, including cache reuse and expiry, malformed-packet rejection, inbound replies, bounded timeout, and keyboard cancellation.

The same target separately requires four successful Ping replies from QEMU user networking's local router `10.0.2.2`, then checks image integrity and continued shell operation.

`make test-network-qemu` includes both the raw-frame driver regression and this protocol acceptance, and `make test` includes the complete network suite.

Successful runs leave no packet capture or debug log behind, while failures retain them under `build/test-artifacts/`.
