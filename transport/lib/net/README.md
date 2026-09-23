# Network Library Boundary

Network-library implementations compile as modern C with fatal project warnings, while application-facing headers remain compatible with strict C90.

The base object group contains `raw.c`, `platform.c`, and `time.c` and is linked into raw-frame consumers such as `netdiag` and `test_network` as well as higher-level network applications.

The IPv4 object group is separate and is linked into `ping`, `netcat`, `ssh`, and the strict-C90 `test_net_protocol` guest probe, so raw diagnostics do not acquire application-protocol state or code.

The build rejects a network source that is missing from both groups, appears in both groups, or is named by a group but absent from the source tree.

`net.h` defines the stable C90 application contract and library-level errors for Ethernet, ARP, static IPv4, and ICMP Echo, while `internal.h` owns modern-C integer-width checks and fixed application-state structures.

The platform layer supplies `net_platform.h`, wrap-safe relative-time helpers, production monotonic and random adapters, and the nonblocking cancellation callback adapter.

The raw transport supplies `raw.h`, shared `raw.def` layout constants, and modern-C wrappers for the three polling NE2000 frame syscalls.

Stage D1 provides the public boundary, fixed state, and deterministic backend.

Stage D2 provides alignment-safe network-byte-order helpers, the Internet checksum, canonical dotted-decimal IPv4 parsing, static configuration validation, raw-device contract validation, and transactional one-time initialization.

Stage D3 provides private Ethernet II frame construction and classification, destination and source validation, explicit zero padding, normalized-frame length checks, and deterministic host tests.

Stage D4 provides private Ethernet/IPv4 ARP parsing, request and reply construction, a four-entry expiring cache, bounded retry and request throttling, on-link resolution, static next-hop selection, and the public `net_poll` service loop.

The Ethernet receive view includes the complete data field, including padding, while the ARP parser consumes only its validated 28-byte message.

The polling loop dispatches validated unfragmented IPv4 datagrams to ICMP Echo, and `net_ping` resolves the next hop and waits for a fully matched reply within one overall deadline.

The IPv4 and ICMP parsers use declared protocol lengths rather than Ethernet padding, reject unsupported options and fragments, and keep protocol state in the application-owned context.

The `ping` application and guest protocol probe are strict C90, while `ipv4.c`, `icmp.c`, and `poll.c` are modern C; both deterministic host tests and QEMU protocol acceptance cover the implemented stack.

ARP learning is unauthenticated, so a matching solicited reply reduces accidental or unsolicited cache changes but does not establish peer identity; address-conflict defense is not implemented.
