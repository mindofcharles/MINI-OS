# MINI-OS Code Structure

This document only describes code/file responsibilities.

## 1. Boot Sources

- `OS_src/boot/boot.asm`
  - EDD detection, bounded one-sector reads, retries, disk resets, and CHS fallback
  - conventional and extended physical-memory checks before the kernel is loaded or protected-mode fixed regions are used
  - fast A20 enablement and physical alias verification before protected mode
  - fixed location for the host-generated per-image boot identity
  - GDT setup and protected-mode jump

## 2. Kernel Entry and Global Constants

- `OS_src/kernel/main.asm`
  - kernel image origin (`[org 0x8000]`) and first-byte jump to `kernel_start`
  - complete kernel-stack clearing and lower-canary initialization before the first call
  - global constants (VGA, ATA, filesystem layout) and compile-time platform-layout assertions
  - global state buffers and scratch variables
  - orders IDT, random-source, PIC, PIT, polling NE2000, filesystem, and shell initialization before entering the prompt
  - includes interrupt/timer/random/syscall/shell/fs/driver/utility modules
- `OS_src/kernel/platform_layout.def`
  - shared boot, kernel, work-buffer, network-buffer, application-image, heap, argument, stack, canary, configured-memory, and firmware-required-memory constants

## 3. Shell Layer

- `OS_src/kernel/shell.asm`
  - command parser
  - command dispatch
  - user-facing error message mapping
  - handlers (`cat`, `edit`, `run`, and command wrappers)
  - `shell_run`: multi-sector executable loader and runner

## 4. Interrupts & System Calls

- `OS_src/kernel/idt.asm`
  - complete IDT table setup (`lidt`) with fatal defaults, exception entries, PIC IRQ entries, and the syscall trap gate
  - `int 0x80` handler for calls 1, 3--7, 12, 14, 15, and 19--31; see `docs/Syscall_ABI.md` for the complete contract
- `OS_src/kernel/interrupts.asm`
  - normalized fatal exception entries for vectors 0--31
  - 8259 remap, explicit masks, startup IRQ/EOI self-test, non-nested dedicated-stack common IRQ entry, and single EOI path
- `OS_src/kernel/timer.asm`
  - PIT channel-0 rate generator and wrapping monotonic-millisecond state
- `OS_src/kernel/random.asm`
  - CPUID/RDRAND capability detection and bounded all-or-nothing secure-random fills

## 5. Driver Layer

- `OS_src/kernel/net.asm`
  - kernel network entry point and ordered aggregator for the NE2000 implementation
- `OS_src/kernel/net/ne2k/definitions.asm`
  - NE2000 registers, packet-memory geometry, timeout constants, test-hook selection, and build-time invariants
- `OS_src/kernel/net/ne2k/lifecycle.asm`
  - polling reset, identification, PROM MAC read, packet-memory setup, device quiescing, and reconfiguration
- `OS_src/kernel/net/ne2k/dma.asm`
  - word-wide Remote DMA reads and writes, recovery dispatch, and fatal-state transition
- `OS_src/kernel/net/ne2k/api.asm`
  - information snapshots, synchronous transmit, one-frame receive, ring validation, wrap, capacity drop, overrun handling, and counters exposed through the raw-frame syscalls
- `OS_src/kernel/net/ne2k/state.asm`
  - persistent driver state, counters, transfer scratch values, and isolated fault-injection markers
- The complete driver uses fixed I/O base `0x300`, keeps configured IRQ9 delivery masked, and consumes shared normalized-frame constants from `transport/lib/net/raw.def`.

- `OS_src/kernel/drivers.asm`
  - checked primary-master ATA PIO read/write helpers (LBA28)
  - blocking and nonblocking Set 1 / US-layout keyboard polling through one translator
  - VGA text-mode output/cursor helpers, including row-crossing backspace

## 6. Utility Layer

- `OS_src/kernel/utils.asm`
  - memory clear/copy
  - string/name copy
  - case-insensitive compare helpers
  - fixed-buffer initialization, application-memory initialization, interrupt-stack boundary exercise, and shared canary verification

## 7. Filesystem Modules

- `OS_src/kernel/fs/bootstrap.asm`: storage-target verification, clean mount, explicit format, and persistent mutation-marker lifecycle
- `OS_src/kernel/fs/path.asm`: path resolve/split/validation/cwd path rebuild
- `OS_src/kernel/fs/dir.asm`: directory entry read/write/clear/scan helpers
- `OS_src/kernel/fs/ops.asm`: high-level create/remove/rename logic
- `OS_src/kernel/fs/path_wrappers.asm`: shell-facing path APIs
- `OS_src/kernel/fs/listing.asm`: `ls` rendering
- `OS_src/kernel/fs/alloc.asm`: inode/FAT-block allocation, inode read/write, inode-bitmap and FAT operations
- `OS_src/kernel/fs.asm`: filesystem include aggregator

## 8. Host Tools & User Applications

- `tools/inject_transport.c`
  - checked host C program for deterministic injection of the `transport/` tree at `/transport/`
  - assigns the per-image boot identity and validates image geometry/FAT chains
  - mutates a same-directory temporary copy and atomically replaces the target only after flush, close, and the full image-integrity gate succeed
- `tools/check_image.c` / `tools/check_image.h`
  - reusable read-only boot-identity, mutation-marker, geometry, inode, directory, FAT-chain, reachability, and allocation-ownership verifier
  - command-line checker and the injector's pre-rename commit gate share the same implementation
- `tools/elf2bin.c`
  - host C 32-bit ELF linker and flat binary generator with checked output, object, global-symbol, per-object-section, and relocation capacities
- `tools/check_layout.c`
  - host C verifier for platform-memory consistency, bounds, alignment, containment, adjacency, pairwise non-overlap, and shared raw-frame-plus-alignment capacity
- `tools/transport_manifest.sh`
  - deterministic content manifest for non-generated files and directories injected from `transport/`, used to refresh the image without forcing unrelated binaries to rebuild
- `transport/lib/`
  - `crt0.asm`: C runtime startup file (`_start`)
  - `minilibc.h` / `minilibc.c`: modern-C runtime implementation and heap allocator
  - `platform.h`: C90-compatible monotonic-clock, nonblocking-key, and secure-random declarations
  - `compiler_rt.c`: modern-C unsigned 64-bit division and remainder helpers linked only where required
  - `net/`: modern-C network implementation directory, including raw-frame syscall wrappers, shared C/assembly ABI definitions, wrap-safe time helpers, production platform/cancellation adapters, byte-order and checksum primitives, static IPv4 configuration validation, transactional stack initialization, private Ethernet II framing/classification, ARP parsing/cache/resolution, and polling dispatch
  - `ssh/`: modern-C SSH implementation directory
  - `stdio.h`, `stdlib.h`, `string.h`, `ctype.h`, `limits.h`, `stddef.h`, `assert.h`: standard C header wrappers
- `transport/app.ld`
  - shared high-memory application section placement and complete allocatable-image assertion for the `ld.lld` path
- `transport/apps/`
  - strict C90 application sources (`hello.c`, `calc.c`, `guess.c`, `banner.c`, `vedit.c`, `netdiag.c`)
- `transport/lib_test/`
  - strict C90 executable assertions in `test_string.c`, `test_heap.c`, `test_file.c`, `test_no_space.c`, `test_bss.c`, `test_stack.c`, `test_platform.c`, and `test_network.c`, plus the isolated fail-stop probe `test_guard.c`
- `transport/build/`
  - compiled flat binary outputs (`apps/*.bin`, `lib_test/*.bin`)

## 9. Build Definition

- `Makefile`
  - source path selection
  - build rules for boot/kernel/image/tool/apps
  - strict-C90 application and modern-C library policies with per-application network and SSH object selection
  - shared layout-derived linker, loader-capacity, kernel-reservation, and QEMU-memory values
  - mandatory final-image integrity check
  - normal and network QEMU run targets, raw-network ABI and driver regressions, plus aggregate test and clean targets

## 10. Automated Test Drivers

- `tests/qemu_e2e.py`
  - boots temporary debug images, checks insufficient-memory and A20 failures, triggers both normalized exception-frame forms, verifies positive and unavailable RDRAND configurations, proves every canary-corruption fail-stop branch, checks timer progress and the network launch configuration, drives the shell, asserts application guards and multi-block filesystem behavior, restarts and checks persistent state, injects ATA faults, verifies wrong-device refusal, and exercises the supported QEMU machine/CHS matrix
- `tests/test_build.py`
  - checks clean and incremental builds, C language policy, per-application dependencies, deterministic platform isolation, production-marker absence, layout assertions, parameterized linker limits, both exact-limit flat-binary paths, checker rejection, and every before/after sector-write failure point in the host injector transaction
- `tests/network_phase_b/`
  - deterministic clock/random/cancellation platform and host regression linked separately from production platform code
- `tests/network_phase_c.py` / `tests/network_phase_c/`
  - deterministic QEMU Ethernet peer, failure-only packet captures, raw-frame boundary/reuse/ring-wrap/recovery checks, and host ABI layout regression
- `tests/network_phase_d/`
  - strict-C90 public-header probe plus deterministic backend, byte-order, checksum, IPv4 configuration, device-contract, initialization, failure-atomicity, Ethernet framing, ARP cache/resolution, and polling host regressions
