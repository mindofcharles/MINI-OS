# Network Library Boundary

Network-library implementations compile as modern C with fatal project warnings, while application-facing headers and guest probes remain compatible with strict C90.

## Selected object groups

- The base group contains `raw.c`, `platform.c`, and `time.c` and is selected by raw-frame and higher-level network consumers.
- The IPv4 group contains initialization, address, byte-order, checksum, Ethernet, ARP, IPv4, ICMP, shared service, and polling code and is selected by `ping`, TCP consumers, and protocol probes.
- The TCP group contains `tcp/api.c`, `tcp/connection.c`, and `tcp/timers.c` and is selected only by named TCP consumers and `test_tcp_contract`.
- The shared crypto group currently contains only `../crypto/clear.c` and is selected with TCP, without importing SSH or a hash/HMAC implementation.

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

## TCP foundation

`tcp.h` fixes C90 signatures, error values, configuration defaults, one generation-checked slot, status snapshots, explicit terminal-handle release, and final application-instance teardown.

The public active-open operation currently returns `TCP_ERR_UNSUPPORTED` without allocating a handle, using entropy, starting ARP, or emitting a SYN, and no public stream operation transfers bytes.

Private lifecycle helpers retain terminal errors after partial-progress reporting, separate call waits from a once-captured close-grace deadline, reject unread-data loss, and prevent old handles from reaching a new generation.

Failed termination clears stream storage and per-connection temporary secrets while preserving the handle's cause and the separate instance secret until explicit release and final teardown.

Private owned ARP tasks survive poll-call wait expiry, preserve their original retry and deadline state, and reject stale owners; the existing public resolver remains a synchronous wrapper with unchanged task-cleanup behavior.

The complete TCP context owns two 4 KiB queues inside a 12 KiB static limit, and the lower-layer context remains inside its original 4 KiB limit without another full-frame buffer or a heap dependency.

The target idle hook currently returns unavailable for positive waits, so fake-clock idle tests do not establish a production idle path.

TCP codec, secure handshake, actual byte streams, retransmission, FIN/TIME_WAIT processing, and target idle/wakeup remain unimplemented.

Deterministic lifecycle fixtures and the guarded strict-C90 guest probe validate this foundation, not TCP wire interoperability.
