# Network

Current status: Phase 0 and Phases A through C are complete. Phase D is in progress: D1 through D3 are complete, and D4 is next.

- Phase 0: select and measure the SSH and cryptographic libraries. Complete.

- Phase A: establish the build and system-memory foundation. Complete.

- Phase B: add exceptions, interrupts, monotonic time, cancellation, and secure randomness. Complete.

- Phase C: implement the polling NE2000 raw-frame transport. Complete.

- Phase D: implement Ethernet, ARP, static IPv4, and ICMP. D1 interfaces and test foundations, D2 primitives and initialization, and D3 Ethernet framing and classification are complete; D4 ARP is next.

- Phase E: implement one active TCP connection and `netcat`. Planned.

- Phases F and G: port the selected SSH stack and add a pinned-host remote-command client. Planned.

- Phases H through J: add persistent host trust, UDP/DHCP/DNS, and an interactive SSH terminal. Deferred.
