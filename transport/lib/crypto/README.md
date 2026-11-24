# Shared Cryptographic Boundary

`clear.c` provides compiler-resistant byte clearing without allocation or a dependency on the network, kernel, or SSH library.

The HMAC-SHA256 wrapper uses the Mbed TLS sources prepared by `make deps` or selected via `MBEDTLS_SOURCE`. The upstream sources and license remain outside the injected `transport/` tree.

`mbedtls_mini_os.h` selects one freestanding configuration. Mbed TLS uses the runtime allocator and delegates explicit zeroing to `crypto_clear`. Keep this configuration and one owner per shared symbol when adding another crypto consumer.

Only TCP-selected consumers link these objects. Raw-only and ICMP applications do not acquire them.

The wrapper clears output on failure and distinguishes allocation failure from other HMAC errors. Host tests force both allocation failures and check zeroing before freeing the temporary buffers. The TCP instance secret is owned separately and is cleared at final TCP teardown.
