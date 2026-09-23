# Network

Current status: Phase 0 and Phases A through C are complete. Phase D is in progress: D1 through D5 are complete, and D6 is next.

- Phase 0: select and measure the SSH and cryptographic libraries. Complete.

- Phase A: establish the build and system-memory foundation. Complete.

- Phase B: add exceptions, interrupts, monotonic time, cancellation, and secure randomness. Complete.

- Phase C: implement the polling NE2000 raw-frame transport. Complete.

- Phase D: implement Ethernet, ARP, static IPv4, and ICMP. D1 interfaces and tests, D2 primitives and initialization, D3 Ethernet framing, D4 ARP resolution and polling, and D5 IPv4/ICMP Echo with `ping` and host tests are complete; D6 QEMU protocol acceptance remains.

- Phase E: implement one active TCP connection and `netcat`. Planned.

- Phases F and G: port the selected SSH stack and add a pinned-host remote-command client. Planned.

- Phases H through J: add persistent host trust, UDP/DHCP/DNS, and an interactive SSH terminal. Deferred.
