# Shared Cryptographic Boundary

`clear.c` provides compiler-resistant byte clearing without allocation or a dependency on the network, kernel, or SSH library.

Only TCP-selected consumers link this object; raw-only and ICMP applications do not acquire it.

No hash, HMAC, entropy substitute, or SSH library is provided here yet.

The future TCP PRF must import only the pinned Mbed TLS SHA-256/HMAC source, header, and license closure, with one freestanding configuration and one definition of each shared symbol for TCP and SSH consumers.

Its temporary allocations and allocation-failure cleanup require explicit measurement, while the application-instance secret remains separate from connection state and is cleared only at final TCP teardown.
