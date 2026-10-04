# Network Feasibility Probes

These probes establish whether a current SSH client and cryptographic backend can be cross-compiled for the MINI-OS i386 flat-binary toolchain before the kernel network stack changes.

They are build and measurement artifacts only, and neither the probe libraries nor their deterministic entropy callbacks are linked into the normal operating-system image.

The runtime library and all third-party sources use modern C compilation, while `transport/apps` and `transport/lib_test` retain the existing strict C90 flags.

## Fixed source revisions

The exact tags, commits, and selected license options are in `versions.conf`.

- libssh2 comes from [libssh2/libssh2](https://github.com/libssh2/libssh2).
- Mbed TLS comes from [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls).
- wolfSSH comes from [wolfSSL/wolfssh](https://github.com/wolfSSL/wolfssh).
- wolfSSL comes from [wolfSSL/wolfssl](https://github.com/wolfSSL/wolfssl).

The build verifies that each fixed tag resolves to its recorded commit, requires that commit at `HEAD`, and rejects staged, unstaged, or untracked source changes, so the recorded measurements cannot silently use another upstream state.

No target downloads source automatically because fetching code is separate from building and reviewing it.

## Manifest check

```bash
make network-phase0-check
```

This check requires no third-party checkout and is part of `make test-build`.

It validates the pinned manifest, shared application layout values, deliberately linked compiler helpers, unmistakable test-entropy markers, and shell syntax of both build drivers.

## Selected-candidate build

```bash
make network-phase0-selected \
    LIBSSH2_SOURCE=/path/to/libssh2 \
    MBEDTLS_SOURCE=/path/to/mbedtls
```

The selected build cross-compiles libssh2 with Mbed TLS, links the same objects at the application base from `OS_src/kernel/platform_layout.def` through both the production `ld.lld` application layout and `tools/elf2bin.c`, and writes measurements below `build/network-phase0/libssh2-mbedtls/`.

Both paths enforce the 512 KiB application-image contract, and the `elf2bin` invocation also passes the current checked object, symbol, section, and relocation capacities explicitly.

The build verifies that the uniquely named Mbed TLS configuration is active and rejects accidental fallback to the upstream default configuration.

## Complete comparison build

```bash
make network-phase0 \
    LIBSSH2_SOURCE=/path/to/libssh2 \
    MBEDTLS_SOURCE=/path/to/mbedtls \
    WOLFSSH_SOURCE=/path/to/wolfssh \
    WOLFSSL_SOURCE=/path/to/wolfssl
```

The comparison adds the minimal wolfSSH and wolfCrypt client probe below `build/network-phase0/wolfssh-wolfcrypt/`.

Each candidate directory contains `report.elf`, both flat outputs, `metrics.txt`, `objects.txt`, `runtime-symbols.txt`, and target stack-usage records.

## Host heap and packet probe

```bash
make network-phase0-host-probe \
    LIBSSH2_SOURCE=/path/to/libssh2 \
    MBEDTLS_SOURCE=/path/to/mbedtls
```

The resulting `build/network-phase0/host-probe/heap-probe` connects only when the operator invokes it with an IPv4 address, port, user name, ECDSA public key, and ECDSA private key.

The host build copies `transport.c` into the output directory and applies `libssh2_packet_probe.patch` to that copy, leaving the upstream checkout unchanged.

Run it only against an isolated test account and a loopback-bound SSH server restricted to `ecdh-sha2-nistp256` key exchange, an `ecdsa-sha2-nistp256` host key, `aes128-ctr` encryption, `hmac-sha2-256` integrity, and no compression.

```text
build/network-phase0/host-probe/heap-probe 127.0.0.1 22422 test-user /path/to/client.pub /path/to/client
```

The fixed remote command is `printf phase0-ok`, and success requires exact output, complete cleanup, and zero tracked bytes after release.

This host probe uses the host `arc4random_buf` only to measure the selected libraries and does not define the MINI-OS production entropy backend.

## Generated data

- `metrics.txt` records section sizes, total allocatable bytes, the aligned allocatable address span, both flat sizes, object and symbol pressure, section pressure, relocations, unresolved symbols, maximum static target stack frame, and selected private-structure sizes.
- `runtime-symbols.txt` records every symbol the third-party objects still require from the MINI-OS port.
- `mbedtls-macros.txt` proves that the intended configuration was selected instead of the upstream default.
- `toolchain.txt` records the local compiler, linker, host compiler, and object-inspection tool versions.

Generated files are disposable and are removed by `make clean`.
