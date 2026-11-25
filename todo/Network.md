# Network

Phase 0 through Phase D are complete. E1 through E4 cover the TCP foundation, segment codec, secure active open, and bounded bidirectional streams. E5a covers timed retransmission, E5b covers congestion control and fast recovery, and E5c covers short sends, window credit, and persist. E5d bounded waits are next.

- Phase 0: select and measure the SSH and cryptographic libraries. Complete.

- Phase A: establish the build and system-memory foundation. Complete.

- Phase B: add exceptions, interrupts, monotonic time, cancellation, and secure randomness. Complete.

- Phase C: implement the polling NE2000 raw-frame transport. Complete.

- Phase D: implement Ethernet, ARP, static IPv4, and ICMP. D1 through D5 implementation and host tests, plus D6 deterministic and user-network QEMU protocol acceptance, are complete.

- Phase E will deliver one active TCP connection and `netcat`. The handshake, bounded streams, loss recovery, and small- and zero-window handling work. Idle waits, connection shutdown, and the diagnostic client are still planned.

- Phases F and G: port the selected SSH stack and add a pinned-host remote-command client. Planned.

- Phases H through J: add persistent host trust, UDP/DHCP/DNS, and an interactive SSH terminal. Deferred.
