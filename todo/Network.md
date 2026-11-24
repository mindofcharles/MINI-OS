# Network

Phase 0 through Phase D are complete. E1 through E3 cover the TCP foundation, segment codec, and secure active open. E4 is next.

- Phase 0: select and measure the SSH and cryptographic libraries. Complete.

- Phase A: establish the build and system-memory foundation. Complete.

- Phase B: add exceptions, interrupts, monotonic time, cancellation, and secure randomness. Complete.

- Phase C: implement the polling NE2000 raw-frame transport. Complete.

- Phase D: implement Ethernet, ARP, static IPv4, and ICMP. D1 through D5 implementation and host tests, plus D6 deterministic and user-network QEMU protocol acceptance, are complete.

- Phase E will deliver one active TCP connection and `netcat`. The handshake is working. E4 adds bounded bidirectional byte streams.

- Phases F and G: port the selected SSH stack and add a pinned-host remote-command client. Planned.

- Phases H through J: add persistent host trust, UDP/DHCP/DNS, and an interactive SSH terminal. Deferred.
