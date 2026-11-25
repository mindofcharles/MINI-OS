# Network Library Boundary

Network-library implementations compile as modern C with fatal project warnings, while application-facing headers and guest probes remain compatible with strict C90.

## Selected object groups

- The base group contains `raw.c`, `platform.c`, and `time.c` and is selected by raw-frame and higher-level network consumers.
- The IPv4 group contains initialization, address, byte-order, checksum, Ethernet, ARP, IPv4, ICMP, shared service, and polling code and is selected by `ping`, TCP consumers, and protocol probes.
- The TCP group contains the interface and connection state, timers, segment codec, sequence arithmetic, ISN generation, active open, and bounded streams under `tcp/`. It is selected only by named TCP consumers and `test_tcp_contract`.
- The shared crypto group links secure clearing and the HMAC-SHA256 wrapper and pinned implementation with TCP.

The build rejects every source under the network and shared crypto directories that has no group, belongs to more than one group, or is named by a group but absent from the tree.

Ordinary applications acquire none of these groups, raw diagnostics acquire no IPv4 or TCP state, and ICMP applications acquire no TCP queues or cryptographic objects.

## Implemented Ethernet and IPv4 behavior

`net.h` defines the stable C90 Ethernet, ARP, static-IPv4, and ICMP Echo contract, while `internal.h` owns modern-C width checks and bounded application state.

`raw.h` and `raw.def` define the polling NE2000 frame interface, and `net_platform.h` supplies relative-time, production random, nonblocking cancellation, and bounded idle-hook declarations.

The Ethernet receive view includes padding, while ARP, IPv4, and ICMP consume only their validated message lengths.

ARP uses a four-entry expiring cache, bounded retries, cross-call request throttling, and static next-hop selection.

The shared service loop rejects reentrancy and dispatches validated unfragmented IPv4 to ICMP or a copied private optional transport binding, with no unconditional reference to the TCP group.

`net_ping` includes resolution and Echo waiting inside one call budget and accepts only a completely matched reply.

ARP learning remains unauthenticated, and options, fragment reassembly, ICMP errors, and an idle-shell Echo server are not implemented.

## TCP active open and byte streams

`tcp.h` fixes C90 signatures, error values, configuration defaults, one generation-checked slot, status snapshots, explicit terminal-handle release, and final application-instance teardown.

`tcp_connect` uses production entropy and a keyed ISN before publishing a handle. It can resume through ARP and the three-way handshake with repeated calls or `net_poll`.

One attempt keeps its original connection deadline and SYN retry history across caller waits. Successful transmission of the final ACK commits the handshake. A matching refusal or exhausted protocol budget becomes a retained terminal result and counts as polling work without interrupting other protocols.

The SYN advertises a zero receive window, and the successful final ACK advertises 4 KiB. `tcp_send` copies a partial prefix into a fixed ring, separating unsent and outstanding bytes. `tcp_recv` copies contiguous bytes from another ring and schedules a window update. A full receive queue answers probes without accepting data. Positive wait expiry does not terminate an otherwise live connection.

The shared service applies a positive stream wait deadline before each new raw receive or transmit. Data received by an already started poll remains queued, and pending output can continue in a later service call.

`tcp/stream.c` owns the established stream accounting and uses `tcp/queue.c` for bounded ring copies. `tcp/output.c` shares transmit-frame construction and next-hop resolution with `tcp/open.c`. Neither queue allocates memory nor adds a frame buffer.

`tcp/rto.c` tracks the timed sequence range, estimates RTT from unambiguous acknowledgments, and backs off after successful timeout retries. The retry reads bytes from the existing ring and shares the normal output path and transmit frame. The no-progress limit for a nonempty queue remains independent of each caller wait and the RTO.

`tcp/congestion.c` limits new data using a byte-counted congestion window, with slow start, congestion avoidance, timeout loss reduction, and idle restart. The sender can have multiple segments outstanding within the peer window and the two existing queue bounds.

Three qualified duplicate ACKs can schedule a fast retransmission, and partial ACKs keep recovery active until the original flight is acknowledged. A fast retransmission does not spend the timeout retry budget. The current stream path does not probe a closed peer receive window or process wire-level FIN and EOF.

Private lifecycle helpers retain terminal errors after partial-progress reporting, separate call waits from a once-captured close-grace deadline, reject unread-data loss, and prevent old handles from reaching a new generation.

Failed termination clears stream storage and per-connection temporary secrets while preserving the handle's cause and the separate instance secret until explicit release and final teardown.

Private owned ARP tasks survive poll-call wait expiry, preserve their original retry and deadline state, and reject stale owners; the existing public resolver remains a synchronous wrapper with unchanged task-cleanup behavior.

The complete TCP context owns two 4 KiB queues inside a 12 KiB static limit, and the lower-layer context remains inside its original 4 KiB limit without another full-frame buffer or a heap dependency.

The target idle hook currently returns unavailable for positive waits, so fake-clock idle tests do not establish a production idle path.

## TCP segments

`tcp/codec.h` defines the private segment view and encoding interface. The decoder checks the exact IPv4 payload and issues a borrowed view after validating the header, options, and checksum.

The encoder supports the fixed TCP header and an optional SYN MSS. It can use payload bytes staged in the existing transmit frame.

`checksum.c` accumulates the IPv4 pseudo-header and segment in wire order while carrying odd-byte position across spans. Its contiguous checksum interface remains available to IPv4 and ICMP.

`tcp/sequence.h` exposes modular comparison and half-open range checks for 32-bit TCP sequence numbers.

The registered TCP callback processes segments that match the connection tuple. A repeat of the accepted SYN-ACK requests an ACK using the current stream state. Transmit uses the resolved configured next hop, and sendable data and a pending ACK can wait across poll calls for the owned ARP refresh.

A reset at the next expected receive sequence fails the connection. A non-exact reset inside the advertised receive window requests a rate-limited empty challenge ACK without changing the stream sequence or the peer's window.
