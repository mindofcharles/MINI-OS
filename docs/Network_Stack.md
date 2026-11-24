# MINI-OS Network Stack

## Current scope

The network stack is linked into an application and uses the kernel's polling NE2000 raw-frame system calls; it is not a background kernel service.

Protocol parsing, packet construction, timers, and bounded state are modern C under `transport/lib/net/`, while applications, guest probes, and the application-facing `net.h` and `tcp.h` interfaces remain strict-C90 compatible.

The supported protocol subset is Ethernet II, Ethernet/IPv4 ARP, static-address unfragmented IPv4, ICMP Echo, and one actively opened IPv4 TCP connection with bounded bidirectional byte streams.

TCP can recover from dropped data or acknowledgments within a bounded retry and transmit budget. Congestion control, sender zero-window probing and recovery from a lost window-reopening update, passive open, and wire-level shutdown are not implemented.

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

TCP packets use the configured next hop. If the neighbor entry expires after SYN, TCP retains the accepted SYN-ACK's sequence, MSS, and window while it resolves that next hop. It remains connecting until the ACK succeeds. A repeated SYN-ACK requests an ACK with the current stream sequence, cumulative acknowledgment, and receive window, without restarting the handshake. A reset in response to an unacceptable SYN-SENT ACK is sent only if the next hop is already resolved.

The initial SYN advertises a zero receive window. The final ACK opens the empty 4 KiB receive queue to the peer and hands the connection to the stream engine.

The SYN timer starts at one second and doubles after each retransmission. Each attempt allows one initial SYN and at most five retransmissions with the same port and sequence number.

While the connection is pending, a zero-wait call gives it one service opportunity and returns `TCP_ERR_WOULD_BLOCK` if it is still pending. A positive call budget can return `NET_ERR_TIMEOUT` without stopping the attempt. Resume with the same handle, endpoint, and configuration, or advance it through `net_poll`.

Generations never wrap or restart within an application instance, and exhaustion has a distinct error instead of reusing an old handle.

The default 120,000 ms connection budget starts at the first call and includes setup, ARP, and the handshake. The 300,000 ms close-grace setting remains reserved for wire-level close. Configured durations are positive and at most `0x7FFFFFFF` milliseconds.

A matching refusal, cancellation, device failure, exhausted neighbor retries, exhausted SYN retries, and expiry of the connection budget are observable through `tcp_status` until the terminal record is released.

For a connection-local failure, `net_poll` reports positive work and `tcp_status` preserves the reason. Unrelated Ping and ARP operations can continue. Cancellation and device failures that affect the shared service still return a network error.

On an established connection, `tcp_shutdown_write` and `tcp_close` return `TCP_ERR_UNSUPPORTED`. Use `tcp_abort` to terminate a live attempt or connection locally. It does not send a shutdown segment or overwrite an existing terminal cause.

`tcp_release` accepts only a terminal record. A new connect cannot overwrite an unreleased record. Final `tcp_deinit` requires an empty slot, clears the instance secret and private registration, and disables TCP for that application run.

Only the next application run recreates this storage, and no TCP work can continue after an application returns to the shell.

## TCP byte streams

`tcp_send` copies a prefix of the caller's buffer into the 4 KiB transmit queue. A positive return counts bytes accepted by the library, not bytes transmitted or acknowledged. The queue keeps unsent bytes separate from sent but unacknowledged bytes and frees a prefix only when a valid cumulative ACK covers it. `tcp_status` reports free local transmit space, not the peer's advertised window.

`tcp_recv` copies contiguous stream bytes from the 4 KiB receive queue. Reads and writes may be partial and do not preserve segment boundaries. With no immediate progress, a zero-wait call gives both directions one service opportunity before returning `TCP_ERR_WOULD_BLOCK`. A positive call budget can expire with `NET_ERR_TIMEOUT` without terminating the connection. In either case, a later call or `net_poll` can continue processing the stream.

Positive stream waits check the deadline before starting an application copy or another raw receive or transmit. An already started driver operation may finish later. A received segment can still update the protocol queues, but the expired call cannot claim newly available bytes or space. Unsent acknowledgments remain pending, and a prefix copied before expiry is still reported.

The sender limits data by the peer's current window and the smaller of the peer's usable MSS and 536 bytes, with at most one effective MSS outstanding for now. A future ACK cannot release unsent data. A window report with an older sequence number, or an older acknowledgment at the same sequence number, cannot replace the current window.

An otherwise valid window update may reuse both sequence and acknowledgment values from the last report because the peer can free buffer space without advancing either value. Those two fields alone cannot order such reports. See [RFC 9293 section 3.8.6](https://www.rfc-editor.org/rfc/rfc9293.html#section-3.8.6).

The receiver drops out-of-order data and acknowledges the next byte it needs. It trims already received prefixes and data beyond its advertised right edge. A full queue advertises zero and answers a peer probe without consuming a byte. Reading data schedules a window update, which can be sent on its own or carried by outgoing data. Space is not granted to the peer until the corresponding window is transmitted.

An established connection accepts a reset at its next expected receive sequence. A reset elsewhere in the advertised window requests an empty challenge ACK, at most once per 500 ms per connection. Other resets are ignored, including non-exact resets when the window is zero. A challenge does not carry queued application data. See [RFC 9293 section 3.10.7.4](https://www.rfc-editor.org/rfc/rfc9293.html#section-3.10.7.4) for the reset checks.

The stream does not consume FIN or report wire-level EOF yet. An accepted reset or other terminal failure remains visible through the handle, including when an I/O call already returned a positive byte count.

## TCP data retransmission

The data retransmission timeout (RTO) starts at 1,000 ms, or 3,000 ms if a shorter SYN timer expired while waiting for the SYN-ACK. The fallback applies even if ARP delayed the SYN retry and the SYN-ACK arrived before another SYN went on the wire. Waiting for ARP to send the final ACK after a timely SYN-ACK is not a SYN timeout.

The sender times the oldest unacknowledged range and retains its bytes in the transmit ring. Successful transmissions start or back off the timer, and an ACK that advances the cumulative sequence restarts it if bytes remain in flight. New sends while the timer is running do not postpone the older bytes. The timer's recorded sequence boundary excludes bytes first sent after it was armed.

A retry uses the original sequence numbers and only bytes inside the peer's current receive window. It reads the current range after ARP refresh, without retaining a prepared packet. A blocked or deferred send does not spend a retry, and a successful retry does not add to the flight size. Timeout retries double the RTO up to 60 seconds.

After five retries without cumulative ACK progress, the next eligible expiry allows the normal single input opportunity before reporting `TCP_ERR_RETRIES`. An advancing ACK or a valid window closure can make that expiry inapplicable. A duplicate ACK that leaves the window open does not postpone failure or grant another receive opportunity.

Fresh data with an unambiguous acknowledgment updates the RTT estimate. ACKs after a retransmission do not supply an ambiguous sample. The calculated RTO is bounded to 1–60 seconds. See [RFC 6298](https://www.rfc-editor.org/rfc/rfc6298.html#section-2).

The independent `transmit_timeout_ms` limit defaults to 300,000 ms. It runs while the transmit queue is nonempty, including when no bytes have reached the wire. Only cumulative ACK progress restarts it while bytes remain; a drained queue disarms it. A caller timeout, another application write, a duplicate ACK, or a window-only update cannot restart it. Expiry is retained as `TCP_ERR_TRANSMIT_TIMEOUT`.

Before starting a raw TCP transmit, the Ethernet layer checks both the no-progress deadline and any positive caller wait against the same clock reading. The no-progress deadline also applies to zero-wait calls, and an ACK processed at or after that deadline cannot revive the connection.

No ordinary retries run against a closed peer receive window. Until sender probing is implemented, the connection needs a valid reopening update to resume in that case, and the independent no-progress limit still applies.

## Internal ownership and resource bounds

The shared service step provides bounded receive, timer, cancellation, and packet dispatch under one reentrancy guard without an unconditional TCP reference in the IPv4 object group.

Private ARP tasks retain their next hop, owner generation, original resolution budget, attempt count, and request timing across poll-call boundaries, while the public synchronous resolver still finishes its task when that call returns.

Synchronous resolution and Ping also finish their task when startup crosses the operation deadline, so a timeout cannot leave the application permanently busy or terminate an otherwise active TCP lifecycle record.

Stale generations cannot start or finish another connection's task, and cache expiry and re-resolution use the existing bounded ARP policy rather than a permanently cached neighbor MAC.

After connection, sendable data and pending ACKs can share a fresh next-hop lookup while receive and cancellation continue. Repeated requests do not restart an unresolved lookup's 120,000 ms budget. ARP attempt exhaustion can end it sooner. A resolved lookup is complete even if the call deadline defers the transmit. Resolution timeout for ordinary output is retained as `TCP_ERR_NEIGHBOR_TIMEOUT`. If only a reset challenge needs the result, resolution timeout abandons the challenge without terminating the connection.

A valid pure ACK releases an owned lookup immediately if there is no longer any data or acknowledgment ready to send. If a retry becomes due later, it can start a new lookup.

If a stream wait expires before sending an ARP or ICMP reply, the shared service can defer that request using the existing receive frame. It sends and counts the reply work on a later service opportunity without allocating another frame.

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

`make test-network-tcp` runs the TCP host suite normally and with AddressSanitizer and UndefinedBehaviorSanitizer, compiles the target TCP objects, and builds the strict-C90 guest probe. It covers the handshake, ring and sequence wrap, partial ACKs, large binary streams, closure and reopening of the receive window, retransmission timing, neighbor refresh, and terminal causes.

`make test-network-tcp-qemu` verifies handshake retransmission, lost final ACK recovery, refusal, and the no-entropy path through QEMU's NE2000 device. A controlled Ethernet peer and the guest also check exact streams of 7,351 and 7,129 bytes in opposite directions. A separate run drops the first data segment and requires the same sequence and bytes before acknowledging the retry. The test also checks gateway routing, an i386 HMAC known-answer vector, repeated application runs, guarded shell return, and image integrity.

`make test-network-d6` boots temporary debug images and tests exact ARP and ICMP wire packets through QEMU's NE2000 model against a controlled Ethernet peer, including cache reuse and expiry, malformed-packet rejection, inbound replies, bounded timeout, and keyboard cancellation.

The same target separately requires four successful Ping replies from QEMU user networking's local router `10.0.2.2`, then checks image integrity and continued shell operation.

`make test-network-qemu` includes the raw-frame driver, ARP/ICMP, TCP handshake and stream, and general end-to-end regressions. `make test` additionally includes the host, sanitizer, ABI, and build regressions.

Successful runs leave no packet capture or debug log behind, while failures retain them under `build/test-artifacts/`.
