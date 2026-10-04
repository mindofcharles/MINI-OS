# MINI-OS Network Stack

## Current scope

The network stack is linked into an application and uses the kernel's polling NE2000 raw-frame system calls; it is not a background kernel service.

Protocol parsing, packet construction, timers, and bounded state are modern C under `transport/lib/net/`, while applications, guest probes, and the application-facing `net.h` and `tcp.h` interfaces remain strict-C90 compatible.

The supported application protocol subset is Ethernet II, Ethernet/IPv4 ARP, static-address unfragmented IPv4, and ICMP Echo Request and Reply.

The TCP interface and lifecycle foundation exist, but TCP packet encoding, handshaking, stream transfer, retransmission, and wire-level closing are not implemented.

UDP, DHCP, DNS, SSH, IPv4 options, fragment reassembly, and ICMP error processing are not implemented.

## Configuration and use

`ping.bin` uses address `10.0.2.15`, mask `255.255.255.0`, and gateway `10.0.2.2`, with the NE2000 device at I/O base `0x300`, IRQ 9, and MAC address `52:54:00:12:34:56` in the supported QEMU run target.

From the MINI-OS shell after `make run-network`, run `run /transport/build/apps/ping.bin 10.0.2.2` to send four bounded Echo Requests and print a per-request result plus a summary.

The public `net_init`, `net_poll`, `net_resolve_arp`, and `net_ping` calls use relative deadlines, distinguish timeout from keyboard cancellation and device failure, and allow only one service operation per application context.

The default ARP cache has four entries, a 60-second lifetime, bounded retries, and at most one ARP Request per second for the same application context.

The network context is created afresh for each application run, so ARP entries do not survive application exit.

## Packet behavior and limits

Transmit frames are explicitly zero-padded to the 60-byte Ethernet minimum, while IPv4 total length excludes that padding.

The IPv4 transmitter uses a 20-byte header, a 1,500-byte MTU, TTL 64, and an atomic datagram with DF set and identification zero.

The receiver validates declared IPv4 lengths and header checksums, rejects options and fragments, and dispatches only unicast packets addressed to the configured local address.

ICMP performs its own message validation, while protocol 6 is accepted only through a private optional transport registration and is currently consumed as unsupported by the TCP foundation.

An outbound Ping succeeds only for an Echo Reply with the expected addresses, identifier, sequence, payload length, and payload bytes.

The stack responds to a valid unicast Echo Request only while a network application is polling; an idle MINI-OS shell is not continuously pingable.

ARP is unauthenticated, and neither address-conflict defense nor protection against a syntactically valid forged ARP Request is provided.

## TCP foundation and contract

Include `net/tcp.h` after initializing the network context, and initialize a new `tcp_handle` to `TCP_INVALID_HANDLE`.

The current `tcp_connect` validates its arguments and returns `TCP_ERR_UNSUPPORTED` without allocating a handle, consuming entropy, resolving a neighbor, or sending a SYN, so applications cannot yet establish a TCP connection.

The foundation defines one occupied slot, generation-checked value handles, retained terminal results, explicit release, bounded connection storage, and independently tested lifecycle helpers rather than providing a simulated successful connection.

Generations never wrap or restart within an application instance, and exhaustion has a distinct error instead of reusing an old handle.

The locked stream contract gives positive send results precedence over a failure detected after that prefix was accepted into library-owned storage, with the later cause retained for subsequent I/O and non-consuming `tcp_status` queries.

An ordinary call timeout is not a terminal connection error, and a positive-capacity receive may return zero only for drained orderly EOF, not temporary no-progress.

These stream rules are interface contracts; the current public send and receive functions do not transfer bytes.

The configuration defaults are a 120,000 ms connect resource budget, a 300,000 ms close-grace budget, and a 300,000 ms pending-transmit resource budget, each positive and at most `0x7FFFFFFF` milliseconds.

The pending-transmit setting is reserved for the byte-stream engine and does not currently schedule a transmit timer.

The close-call wait budget is separate from the close-grace budget, which is captured once on the first accepted shutdown or close and cannot be restarted by repeated calls.

For a live internal lifecycle record, zero-timeout close advances once and returns `TCP_ERR_WOULD_BLOCK` if incomplete, while an ordinary nonzero wait expiry preserves that record and a close-grace expiry retains `TCP_ERR_CLOSE_TIMEOUT`.

Full close cannot silently discard unread receive bytes or skip EOF observation, and `tcp_abort` terminates without releasing the handle or overwriting an earlier terminal cause.

`tcp_release` accepts only a terminal record, and a new connect cannot overwrite an unreleased record; final `tcp_deinit` requires an empty slot, clears the instance secret and private registration, and permanently disables TCP for that application instance.

Only the next application run recreates this storage, and no TCP work can continue after an application returns to the shell.

## Internal ownership and resource bounds

The shared service step provides bounded receive, timer, cancellation, and packet dispatch under one reentrancy guard without an unconditional TCP reference in the IPv4 object group.

Private ARP tasks retain their next hop, owner generation, original resolution budget, attempt count, and request timing across poll-call boundaries, while the public synchronous resolver still finishes its task when that call returns.

Synchronous resolution and Ping also finish their task when startup crosses the operation deadline, so a timeout cannot leave the application permanently busy or terminate an otherwise active TCP lifecycle record.

Stale generations cannot start or finish another connection's task, and cache expiry and re-resolution use the existing bounded ARP policy rather than a permanently cached neighbor MAC.

The lower-layer context remains at most 4 KiB and contains the original two 1,514-byte frame buffers, while the complete separate TCP context is limited to 12 KiB and owns two 4 KiB byte queues without heap allocation.

The shared cryptographic boundary currently contains only compiler-resistant clearing; no TCP PRF, hash/HMAC import, or secure active-open implementation exists yet.

Raw-only and ICMP applications do not link TCP queues or shared cryptographic objects.

The platform declares a bounded `net_idle` hook, but the target returns `SYS_ERR_UNAVAILABLE` for a positive duration and performs no spin fallback; zero duration returns without waiting.

This is not an implemented target idle/wakeup path, and the deterministic backend's fake-clock advancement does not establish production waiting behavior.

## Automated validation

`make test-network-host` runs deterministic backend, timing, cache, parser, and packet-construction tests without QEMU.

It also checks the TCP foundation through private lifecycle fixtures, including retained errors, close-budget separation, stale handles, persistent ARP tasks, and the public unsupported-operation gates.

`make test-network-sanitizers` instruments the complete currently implemented TCP foundation and shared packet/ARP service with AddressSanitizer and UndefinedBehaviorSanitizer, rather than checking only a packet parser.

`make test-network-e1-qemu` runs the strict-C90 public-contract probe twice under each supported RDRAND-on and RDRAND-off CPU configuration, checks guarded return to the shell and image integrity, and does not claim a TCP handshake or interoperability result.

`make test-network-d6` boots temporary debug images and tests exact ARP and ICMP wire packets through QEMU's NE2000 model against a controlled Ethernet peer, including cache reuse and expiry, malformed-packet rejection, inbound replies, bounded timeout, and keyboard cancellation.

The same target separately requires four successful Ping replies from QEMU user networking's local router `10.0.2.2`, then checks image integrity and continued shell operation.

`make test-network-qemu` includes the raw-frame driver regression, ARP/ICMP acceptance, TCP-foundation guest probe, and general end-to-end checks, while `make test` additionally includes the host, sanitizer, ABI, and build regressions.

Successful runs leave no packet capture or debug log behind, while failures retain them under `build/test-artifacts/`.
