# Network

Phase 0 and Phases A through D are complete. Phase E1 delivered the TCP foundation and E2 added the segment codec and sequence helpers. E3 is next.

- Phase 0: select and measure the SSH and cryptographic libraries. Complete.

- Phase A: establish the build and system-memory foundation. Complete.

- Phase B: add exceptions, interrupts, monotonic time, cancellation, and secure randomness. Complete.

- Phase C: implement the polling NE2000 raw-frame transport. Complete.

- Phase D: implement Ethernet, ARP, static IPv4, and ICMP. D1 through D5 implementation and host tests, plus D6 deterministic and user-network QEMU protocol acceptance, are complete.

- Phase E will deliver one active TCP connection and `netcat`. E1 and E2 are complete. E3 covers secure active open and the first TCP handshake.

- Phases F and G: port the selected SSH stack and add a pinned-host remote-command client. Planned.

- Phases H through J: add persistent host trust, UDP/DHCP/DNS, and an interactive SSH terminal. Deferred.
