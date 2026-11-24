# MINI-OS Network Stack

## Current scope

The network stack is linked into an application and uses the kernel's polling NE2000 raw-frame system calls; it is not a background kernel service.

Protocol parsing, packet construction, timers, and bounded state are modern C under `transport/lib/net/`, while applications, guest probes, and the application-facing `net.h` and `tcp.h` interfaces remain strict-C90 compatible.

The supported protocol subset is Ethernet II, Ethernet/IPv4 ARP, static-address unfragmented IPv4, ICMP Echo, and active opening of one IPv4 TCP connection.

TCP stream transfer, passive open, and wire-level connection shutdown are not implemented.

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

ICMP performs its own message validation. Protocol 6 is accepted only through a private optional transport registration.

An outbound Ping succeeds only for an Echo Reply with the expected addresses, identifier, sequence, payload length, and payload bytes.

The stack responds to a valid unicast Echo Request only while a network application is polling; an idle MINI-OS shell is not continuously pingable.

ARP is unauthenticated, and neither address-conflict defense nor protection against a syntactically valid forged ARP Request is provided.

## TCP segment processing

The private decoder validates the exact IPv4 payload, including the TCP data offset, option bounds, and mandatory IPv4 pseudo-header checksum. The returned payload view borrows bytes from the received frame.

The decoder handles EOL, NOP, and MSS and skips well-formed options it does not support. It accepts a usable MSS on SYN and takes the smallest nonzero value if the option appears more than once. Header bytes after EOL must be zero.

The private encoder writes a bounded TCP segment directly into the caller's IPv4 transmit space. It can checksum payload bytes already staged there and can emit a single MSS option on SYN.

Sequence numbers use unsigned 32-bit modular arithmetic. The helpers expose the ambiguous half-space comparison and count each SYN and FIN as one sequence position in addition to any payload.

The registered TCP callback counts malformed segments and passes validated segments to the active connection.

## TCP active open and handle ownership

Include `net/tcp.h`, initialize the network with `net_init`, and set a new `tcp_handle` to `TCP_INVALID_HANDLE`.

`tcp_connect` resolves the configured next hop, sends a SYN with a 536-byte MSS, and returns zero after accepting the peer's SYN-ACK and successfully sending the final ACK. The final transmission is the commit point. Its last deadline check happens before sending, so a tick during a successful synchronous send cannot retroactively fail the handshake.

TCP control packets use the configured next hop. If the neighbor entry expires after SYN, TCP retains the accepted SYN-ACK's sequence, MSS, and window while it resolves that next hop. It remains connecting until the ACK succeeds. A repeated SYN-ACK requests the same ACK after establishment. A reset in response to an unacceptable SYN-SENT ACK is sent only if the next hop is already resolved.

The advertised receive window remains zero. The stack answers a peer's window probe with the unchanged cumulative ACK and window, without accepting data or FIN. Positive-length sends and receives on an established connection return `TCP_ERR_UNSUPPORTED`.

The SYN timer starts at one second and doubles after each retransmission. Each attempt allows one initial SYN and at most five retransmissions with the same port and sequence number.

While the connection is pending, a zero-wait call gives it one service opportunity and returns `TCP_ERR_WOULD_BLOCK` if it is still pending. A positive call budget can return `NET_ERR_TIMEOUT` without stopping the attempt. Resume with the same handle, endpoint, and configuration, or advance it through `net_poll`.

Generations never wrap or restart within an application instance, and exhaustion has a distinct error instead of reusing an old handle.

The default 120,000 ms connection budget starts at the first call and includes setup, ARP, and the handshake. The 300,000 ms close-grace and pending-transmit settings are reserved for the corresponding wire-level features. Configured durations are positive and at most `0x7FFFFFFF` milliseconds.

A matching refusal, cancellation, device failure, exhausted neighbor retries, exhausted SYN retries, and expiry of the connection budget are observable through `tcp_status` until the terminal record is released.

For a connection-local failure, `net_poll` reports positive work and `tcp_status` preserves the reason. Unrelated Ping and ARP operations can continue. Cancellation and device failures that affect the shared service still return a network error.

On an established connection, `tcp_shutdown_write` and `tcp_close` return `TCP_ERR_UNSUPPORTED`. Use `tcp_abort` to terminate a live attempt or connection locally. It does not send a shutdown segment or overwrite an existing terminal cause.

`tcp_release` accepts only a terminal record. A new connect cannot overwrite an unreleased record. Final `tcp_deinit` requires an empty slot, clears the instance secret and private registration, and disables TCP for that application run.

Only the next application run recreates this storage, and no TCP work can continue after an application returns to the shell.

## Internal ownership and resource bounds

The shared service step provides bounded receive, timer, cancellation, and packet dispatch under one reentrancy guard without an unconditional TCP reference in the IPv4 object group.

Private ARP tasks retain their next hop, owner generation, original resolution budget, attempt count, and request timing across poll-call boundaries, while the public synchronous resolver still finishes its task when that call returns.

Synchronous resolution and Ping also finish their task when startup crosses the operation deadline, so a timeout cannot leave the application permanently busy or terminate an otherwise active TCP lifecycle record.

Stale generations cannot start or finish another connection's task, and cache expiry and re-resolution use the existing bounded ARP policy rather than a permanently cached neighbor MAC.

A post-connect ACK can also wait for a fresh next hop. There is one pending ACK per connection, and repeated requests for it do not restart its 120,000 ms neighbor budget. ARP attempt exhaustion can end it sooner. A failed refresh is retained as `TCP_ERR_NEIGHBOR_TIMEOUT`.

The shared receive service treats successful NE2000 recovery from a receive overrun or receive DMA timeout as a dropped frame. Other receive errors keep their existing failure mapping. A transmit error still does not confirm delivery or complete a handshake.

The lower-layer context remains at most 4 KiB and contains the original two 1,514-byte frame buffers, while the complete separate TCP context is limited to 12 KiB and owns two 4 KiB byte queues without heap allocation.

The first TCP attempt obtains a 32-byte instance secret from the production random service. Each new attempt selects a random ephemeral port in `49152..65535` and derives its sequence number from HMAC-SHA256 over the IPv4 endpoint tuple plus a wrapping time and generation component.

Entropy, HMAC, and its two temporary allocation failures are reported before handle publication or ARP transmission. `make deps` prepares the pinned HMAC sources and upstream license outside the injected guest tree.

Raw-only and ICMP applications do not link TCP queues or shared cryptographic objects.

The platform declares a bounded `net_idle` hook, but the target returns `SYS_ERR_UNAVAILABLE` for a positive duration and performs no spin fallback; zero duration returns without waiting.

This is not an implemented target idle/wakeup path, and the deterministic backend's fake-clock advancement does not establish production waiting behavior.

## Automated validation

`make test-network-host` runs deterministic backend, timing, cache, parser, and packet-construction tests without QEMU.

It checks the TCP lifecycle foundation and uses independent wire vectors for the codec, including the IPv4 receive and transmit-buffer paths.

`make test-network-e3` runs the network host suite and its AddressSanitizer and UndefinedBehaviorSanitizer build, compiles the target TCP objects, and builds the strict-C90 guest probe. It covers HMAC known answers and allocation faults, entropy failures, exact SYN and ACK packets, retries, neighbor refresh, and terminal causes.

`make test-network-e3-qemu` compares handshake frames against a controlled Ethernet peer on QEMU's NE2000 device. It checks retransmission, lost final ACK recovery, different ingress and egress MAC addresses, zero-window probe acknowledgments, refusal, and zero transmitted frames when RDRAND is unavailable. It also checks an i386 HMAC known-answer vector, repeated app runs, guarded shell return, and image integrity.

`make test-network-d6` boots temporary debug images and tests exact ARP and ICMP wire packets through QEMU's NE2000 model against a controlled Ethernet peer, including cache reuse and expiry, malformed-packet rejection, inbound replies, bounded timeout, and keyboard cancellation.

The same target separately requires four successful Ping replies from QEMU user networking's local router `10.0.2.2`, then checks image integrity and continued shell operation.

`make test-network-qemu` includes the raw-frame driver, ARP/ICMP, TCP active-open, and general end-to-end regressions. `make test` additionally includes the host, sanitizer, ABI, and build regressions.

Successful runs leave no packet capture or debug log behind, while failures retain them under `build/test-artifacts/`.
