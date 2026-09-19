# MINI-OS Build and Run

This document only covers build, image generation, and local execution.

## 1. Prerequisites

Required tools:

- `nasm`
- `cc` for the four modern-C host tools
- `clang` for the freestanding runtime, applications, and tests
- `ld.lld` when available; otherwise the checked `elf2bin` fallback is selected
- `qemu-system-i386`
- shell tools used by `Makefile` (`awk`, `dd`, `expr`, `find`, `grep`, `mkdir`, `printf`, `rm`, `tr`, `wc`)
- Python 3.9 or newer for the automated build and QEMU tests

## 2. Source and Output Paths

Current source layout:

- bootloader source: `OS_src/boot/boot.asm`
- kernel entry source: `OS_src/kernel/main.asm`

Build outputs:

- `build/boot.bin`
- `build/kernel.bin`
- `build/inject_transport`
- `build/elf2bin`
- `build/check_image`
- `build/check_layout`
- `build/mini_os.img`
- `build/transport/apps/*.o`
- `build/transport/lib_test/*.o`
- `transport/build/apps/*.bin`
- `transport/build/lib_test/*.bin`

## 3. Build Commands

From project root:

```bash
make clean
make
```

What happens:

- Build and run the shared platform-memory layout checker.
- Build the modern-C host tools and runtime library.
- Compile every application and test with strict C90 diagnostics.
- Link each flat binary at `0x00100000` with the checked linker script and `ld.lld`, or automatically use the parameterized `elf2bin` path when `ld.lld` is unavailable.
- Assemble the kernel and compute its boot-time sector count.
- Create a raw image of exactly 4,471 sectors from the shared `FS_DATA_START_LBA + FS_DATA_BLOCK_COUNT` layout constants.
- Assert the resulting byte size and write the boot and kernel sectors.
- Assign a nonzero 48-bit image identity, initialize a zeroed fresh filesystem, and inject the sorted `transport/` tree in a same-directory temporary copy.
- Flush, close, and fully validate the copy before atomically renaming it over the image.
- Run `build/check_image` as a mandatory final step, and fail the image build for any invalid identity, unfinished mutation, geometry, inode, FAT chain, duplicate ownership, unreachable allocation, or directory inconsistency.

Host metadata such as `.DS_Store`, `._*`, `Thumbs.db`, and `.git` is excluded.

An image or host-file I/O failure makes the injector and build return nonzero.

On any injector failure, including a failure reported after a sector flush or by the full pre-rename integrity gate, the original image remains byte-for-byte unchanged.

The temporary copy is removed when cleanup succeeds, and a cleanup failure is reported.

A nonzero invalid or unfinished superblock is rejected rather than being reformatted.

The injector requires a regular non-symlink target with exactly one hard link, preserves its permission bits, and assumes that no other process concurrently replaces it.

An abrupt process or host shutdown can leave an uncommitted `.inject-*` sidecar, which does not replace the target and may be removed after confirming that no injector is running.

To build one application:

```bash
make app APP=hello.c
```

Objects retain their source path below `build/transport/`, so an application and a library test may safely share a basename.

### Optional Network Feasibility Build

The optional network feasibility build uses clean external checkouts at fixed commits and never downloads or links them into the normal image.

```bash
make network-phase0-check

make network-phase0 \
    LIBSSH2_SOURCE=/path/to/libssh2 \
    MBEDTLS_SOURCE=/path/to/mbedtls \
    WOLFSSH_SOURCE=/path/to/wolfssh \
    WOLFSSL_SOURCE=/path/to/wolfssl
```

The pinned revisions, probe configuration, generated outputs, and host-probe procedure are documented with the reproducible tooling in [`tools/network_phase0/README.md`](../tools/network_phase0/README.md).

The deterministic platform regression runs entirely on the host and does not require external sources:

```bash
make test-network-host
```

This target creates `build/network-phase-b/platform_test` by substituting a test-only clock, random source, and cancellation callback for the production syscall adapter. Its marker is checked for absence from `build/mini_os.img` by the build regression.

## 4. Run in QEMU

```bash
make run
```

Equivalent current action:

```bash
qemu-system-i386 -m 4M -drive file=build/mini_os.img,format=raw,if=ide,index=0,media=disk
```

The 4 MiB value is derived from `PLATFORM_CONFIGURED_MEMORY_BYTES` in `OS_src/kernel/platform_layout.def`.

The bootloader separately checks the firmware-reported spans required through `0x00095000` below the legacy-memory hole and through `0x001CB000` above one MiB; the full contract is documented in [`Memory_Layout.md`](Memory_Layout.md).

The network-oriented QEMU configuration can be launched with `make run-network`, which additionally selects QEMU TCG with RDRAND enabled and attaches a user-mode backend to an NE2000 ISA device at I/O base `0x300`, IRQ 9, and MAC address `52:54:00:12:34:56`.

The kernel probes that device without making it a boot requirement, keeps IRQ9 masked, and exposes complete raw Ethernet frames through the polling syscalls documented in [`Network_Raw_Transport.md`](Network_Raw_Transport.md).

The strict-C90 diagnostic can be run after `make run-network`:

```text
run /transport/build/apps/netdiag.bin
```

The current secure-random syscall requires the advertised RDRAND feature. `make run-network` provides the accepted positive emulator configuration, while CPUs without that feature receive an explicit unavailable error and never fall back to the runtime `rand()` generator.

The explicit IDE index is part of the current driver contract because protected-mode filesystem I/O addresses the primary ATA channel's master device directly after the BIOS loads the kernel.

Before mounting, the kernel compares the boot-code prefix, per-image identity, and an immutable kernel-code sample with that target.

A mismatch halts without writes.

## 5. Kernel Size Guard and Boot Reads

The Makefile checks that kernel size does not exceed reserved area (`100` sectors).

If exceeded, build stops with an explicit error.

Before loading the kernel, the boot sector rejects firmware that cannot provide the low and high contiguous RAM spans required by the shared physical layout.

The boot sector then probes EDD and reads the kernel sectors individually with retries; if EDD is unavailable or fails, it retries through a CHS fallback.

Each request uses offset zero and advances the destination segment, including at the 100-sector build limit, so no request crosses a 64 KiB offset boundary.

After loading the kernel, the boot sector enables the fast A20 gate and verifies non-aliasing with two bytes exactly one MiB apart before entering protected mode.

Application binaries have a separate 512 KiB build-time limit matching `0x00100000..0x0017FFFF`, and the linker script includes zero-initialized placement when enforcing that bound.

The fallback `elf2bin` capacities are explicit Make variables with defaults of 256 input objects, 4,096 unique global symbols, 4,096 sections per object, and 32,768 relocations, while its output limit is always the shared 512 KiB application-image size.

## 6. Dependency Tracking

The kernel target depends on every assembly/layout include below `OS_src/kernel/` and the shared raw-frame definition.

The layout checker, runtime, and application targets also depend on the raw-frame definition where applicable, while runtime and application targets depend on all public runtime headers and `syscall.def`.

Ordinary applications link only `crt0` and `minilibc`, the named network applications `netdiag`, `ping`, `netcat`, and `ssh` plus the raw-network executable test additionally link the modern-C network objects and compiler runtime, and only `ssh` links the modern-C SSH objects.

The Makefile itself is also an input to generated tools, objects, binaries, and the final image, so flag or recipe changes trigger the required rebuild.

## 7. Common Build Issues

### Missing `nasm`

Symptom: assembler command not found.

### Missing `qemu-system-i386`

Symptom: `make run` fails to launch emulator.

### Permission-related issues with tools

Symptom: image write or cleanup commands fail.

## 8. Verification Targets

```bash
make check-layout  # verify platform memory sizes, bounds, and non-overlap
make check-image  # verify the current generated image
make network-phase0-check  # verify pinned network-probe policy without external sources
make test-network-abi  # verify every public raw-frame structure offset
make test-network-driver  # exercise NE2000 frames, ring wrap, and recovery in QEMU
make test-network-qemu  # run packet-socket and canonical user-network QEMU checks
make test-network  # run all host, ABI, and QEMU network checks
make test-build   # build policy, corruption rejection, and host write-fault transaction checks
make test-e2e     # QEMU filesystem, persistence, disk-fault, device-safety, and boot-matrix checks
make test         # all automated gates
```

The QEMU test uses a temporary copy of the image and does not mutate `build/mini_os.img`. Details and asserted behavior are in [`Testing.md`](Testing.md).

## 9. Real Hardware

Physical machine flashing/boot instructions are documented separately in:

- `docs/Real_Hardware_Guide.md`
