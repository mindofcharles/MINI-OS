# MINI-OS Limitations

This file documents current limitations only.

## Platform And Runtime

- Single-task execution model that executes raw flat binaries in the 512 KiB image region beginning at `0x00100000`.
- System calls via `int 0x80` for console, heap, file, cursor, monotonic-clock, nonblocking-key, secure-random, and raw-frame services; `Syscall_ABI.md` documents all implemented calls and their register contract.
- Dynamic heap memory allocation (`malloc`, `free`, `realloc`, `calloc`) managed by `sys_brk` in the fixed 256 KiB region `0x00180000..0x001BFFFF`.
- A tested runtime subset exposed through familiar C headers; unsupported functions and reduced contracts are listed in `Library_Support.md`.
- No Ring 3 hardware process isolation; user applications and kernel share Ring 0 flat protected mode.
- No virtual memory or paging.
- Physical memory uses a checked fixed layout with legacy BIOS span checks, but there is no full E820 map parser or physical-page allocator.
- Memory canaries detect selected heap and downward-stack boundary corruption only after control reaches a verification point; they do not prevent writes or provide recovery.
- No interrupt-driven scheduling.
- Only PIT IRQ0 is unmasked; the NE2000 transport deliberately polls with its configured IRQ9 masked, and device IRQ handling beyond the timer is not enabled.
- The monotonic clock is a wrapping 32-bit elapsed-time source with approximately one-millisecond PIT resolution, not a wall clock.
- Secure random bytes require CPUID-advertised RDRAND and return unavailable without a fallback on older CPUs.
- A polling NE2000 raw-frame driver, static IPv4 configuration, Ethernet II framing, ARP, restricted IPv4 packet processing, ICMP Echo, and a strict-C90 `ping` application exist; UDP, TCP, DNS, DHCP, and SSH applications do not.
- The IPv4 implementation rejects options and fragments, does not reassemble datagrams or process ICMP errors, and responds to Echo Requests only while a network application polls; ARP and ICMP have deterministic host coverage but not yet a QEMU protocol acceptance test.
- ARP has no authentication, and address-conflict probing and defense are not implemented.

## Filesystem And Storage

- FAT-chain allocation; files may be fragmented across the data region.
- No journal, automatic rollback/replay, or crash repair exists, although a persistent marker detects an interrupted mutation and forces a read-only halt instead of silently mounting the ambiguous result.
- No standalone fsck/repair command.
- No file permissions model.
- No ownership metadata.
- No timestamp metadata.
- On-disk compatibility/version migration is not defined.

## Device And Driver Layer

- ATA PIO path is polling-based.
- Protected-mode storage supports only an LBA28 disk exposed as the primary ATA channel's master device, and the BIOS boot-drive number is not mapped to a kernel storage device.
- The kernel refuses a target whose boot-code prefix, 48-bit image identity, or kernel-code sample differs from the BIOS-loaded image.
- Images sharing all checked bytes, including clones whose filesystems later diverge, remain indistinguishable, and a 48-bit identity can theoretically collide.
- Disk I/O reports success/failure after bounded waits and `ERR`/`DF` checks, but does not expose richer controller diagnostics to shell-level logic.
- Keyboard input uses blocking and nonblocking controller polling with a limited Set 1 US-layout mapping; keyboard IRQ1 remains masked, and there is no Caps Lock, alternate layout, or asynchronous input state.
- The network driver supports only a fixed NE2000-compatible ISA device at I/O base `0x300`, uses a fixed 16 KiB packet-memory layout, remains polling-only, supports untagged 60-through-1,514-byte frames, and has not been validated on physical boards.

## Robustness And Validation

- Runtime guards validate the structures needed by each operation, and every generated image is fully walked by the host-side read-only checker.
- Kernel mutations are fail-stop rather than atomic because a failure after a persistent operation write may leave partial changes, while the persistent marker blocks further writes and remount.
- Any ATA I/O failure disables later filesystem writes for that boot.
- There is no in-system repair utility.
- A dirty image cannot reach the shell's `format` command, and the injector refuses it, so recovery currently means offline inspection or replacing or rebuilding the image.

## Testing And Tooling

- Automated QEMU tests cover insufficient-memory and A20 failures, normalized exceptions, PIC/IRQ startup invariants, PIT progress, positive and unavailable RDRAND paths, positive and deliberately corrupted memory-canary paths, NE2000 identification, exact raw-frame boundaries, receive-ring wrap, capacity drops, timeout/overrun recovery, library assertions, multi-block file I/O, append/move/remove behavior, persistence, deterministic ATA read/write faults, partial-write exhaustion, wrong-device refusal, three forced-CHS geometries, default PC, and `isapc`.
- Automated build tests cover deterministic platform isolation, timeout wrap behavior, marker absence, layout overlap and invalid firmware/configured-memory boundaries, strict-C90 application and modern-C library separation, per-application network/SSH dependencies, all parameterized `elf2bin` capacities, exact-limit and overflowing images, relevant dependency rebuilds, no-op builds, both flat-binary link paths, dirty-image rejection, every host-injector sector-write failure point before and after flush, and every final transaction stage including the full pre-rename integrity gate.
- Physical-machine compatibility remains unverified because firmware USB-to-legacy-ATA mapping varies and cannot be established by emulator coverage.

## Summary

The system is functional for its current personal-project scope, but reliability and fault tolerance remain limited.

Using it as an experimental environment is reasonable; using it as a trusted storage system is not.
