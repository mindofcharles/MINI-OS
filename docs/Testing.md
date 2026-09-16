# MINI-OS Testing

## Test Targets

```bash
make check-layout
make check-image
make network-phase0-check
make test-network-host
make test-e2e
make test-build
make test
```

`make` first checks the platform memory layout and then runs the read-only image checker after host injection. `make test` runs both build-policy and QEMU end-to-end regressions.

`make test-network-host` builds and runs the separate deterministic clock, random, wrap, delayed-poll, and cancellation regression without linking the production syscall adapter.

`make test-build` first runs the pinned network-feasibility manifest and compiler-helper regression without downloading or compiling external SSH sources.

Tests require Python 3.9 or newer and `qemu-system-i386` in addition to the normal build tools.

## Image Integrity Gate

`tools/check_image.c` verifies the complete image without modifying it:

- boot signature and nonzero per-image identity;
- exact geometry and superblock fields, including a clear mutation marker;
- inode bitmap/type agreement;
- exact, bounded, acyclic FAT chains;
- one owner per allocated data block and no orphan FAT allocation;
- root reachability and exactly one parent link for every other inode;
- NUL-terminated names of at most 26 visible bytes, reserved-name and slash rejection, directory name uniqueness, and entry/inode agreement;
- file-size/block-count agreement without arithmetic wraparound;
- all metadata and data references within image bounds.

Any failure terminates the image build.

## Platform Memory Layout Gate

`tools/check_layout.c` imports the same `OS_src/kernel/platform_layout.def` constants as the assembly boot and kernel paths, while the Makefile derives the application link address, image limit, kernel reservation, and QEMU memory value from that file.

The checker rejects empty or reversed ranges, ranges beyond the 4 MiB configured ceiling, ranges beyond either firmware-checked RAM span, inconsistent declared sizes, insufficient Ethernet frame buffers, malformed argument subranges, misplaced canaries, incorrect alignment, and every pairwise overlap.

The build regression deliberately creates an overlap and separately lowers the conventional, extended, and configured-memory boundaries, requires every invalid definition to be rejected, restores the shared definition, and requires the valid layout to pass again.

## QEMU Filesystem Regression

`tests/qemu_e2e.py` creates a temporary copy of the image and a test-only kernel that mirrors VGA characters to QEMU's debug console. The production kernel and source image are not mutated. The test has explicit timeouts and fails if QEMU hangs or an expected marker is absent.

It boots and asserts:

- A forced A20 verification failure prints its dedicated boot marker and halts before protected mode.
- A deliberately undersized 1 MiB machine prints the dedicated `M` marker and halts before the kernel is loaded.
- The exact `make run-network` TCG, RDRAND, user-network, and NE2000 device configuration reaches the shell with 4 MiB of guest memory.
- Real divide-error and general-protection exceptions reach the normalized fatal handler with the expected vectors and zero or hardware-supplied error code.
- Startup completes only after software-triggered IRQ0, masked IRQ1, and masked slave IRQ8 traverse the common dedicated-stack path with restored registers, segment selectors, direction flag and stack state, exact master/slave EOI counts, and intact canaries.
- The monotonic counter advances while a strict-C90 application repeatedly uses the nonblocking keyboard syscall and filesystem read syscalls.
- A 1,024-byte secure-random request succeeds under `-accel tcg -cpu max,rdrand=on`, while both `-accel tcg -cpu qemu32,rdrand=off` and a CPUID-free `-cpu 486` return unavailable and clear the full destination.
- The existing strict-C90 `hello` application retains its exact greeting and integer-format output before a clean guarded return.
- Exact string, formatting, heap, BSS, syscall, and stream test results are verified.
- Heap exhaustion returns failure, the freed heap remains reusable, and the post-application heap guard remains intact.
- A strict-C90 test actively uses 28 KiB of the 32 KiB application stack, while application, interrupt, and kernel guard regions remain intact.
- Isolated strict-C90 test boots separately corrupt the kernel-stack, interrupt-stack, application-heap, and application-stack canaries, and every case must reach the memory-guard fatal halt instead of another shell prompt.
- Creation, truncation, exact multi-sector write/read, gap zeroing, and append are verified.
- The 26-byte accepted and 27-byte rejected name boundaries are verified together with trailing-slash and root rejection.
- Two-block ordinary and root directory growth, rename, listing, removal, cleanup, and remount with the root still owning two blocks are verified.
- Current-directory and ancestor removal protection, case-only rename, and current-working-directory refresh after moving an ancestor are verified.
- The `vedit test` screen-rendering path is verified.
- Forced-CHS boots under `5/16/63`, `66/4/17`, and `263/1/17` geometries are verified together with default PC and `isapc` machine types.
- Booting from secondary master while primary master separately mismatches the boot-code sample, image identity, or kernel-code sample is refused in every case, and hashes prove both images remain unchanged.
- An invalid-superblock boot halts without formatting and leaves the image hash unchanged.
- An explicit `format` removes the prior tree, recreates `README.TXT`, passes the checker, and remains mountable after reboot.
- A one-shot ATA write error at FAT LBA 103 disables writes in the current boot, causes the checker to report an unfinished mutation, and makes the next boot refuse the mount.
- A one-shot ATA read error on `README.TXT` refuses later writes in that boot, leaves the image hash unchanged and clean, and permits writes after reboot.
- A nearly full image whose two-block write persists its first block before running out of space retains the unfinished marker and refuses remount even though the terminal error was not an ATA failure.
- Bytes, moves, and removals persist across complete QEMU restarts, including deletion of `README.TXT` and verified reuse of its inode 1.
- Image integrity is verified before remount, after cleanup, and after the final remount.

## Build Regression

`tests/test_build.py` copies the repository to a temporary directory and checks:

- a self-contained pinned network-feasibility manifest and compiler-helper regression before any external source is needed;
- a clean default build and mandatory image verification;
- build-time proof that every platform memory range is aligned, firmware-backed, bounded, and non-overlapping;
- a binary-level assertion that the kernel image starts with an executable jump whose destination lies inside the image;
- GNU C11 compilation of `transport/lib` and strict C90 flags for apps/tests;
- strict-C90 parsing of the platform and network-platform public headers;
- deterministic host checks for wrapping elapsed time, maximum duration, delayed polling, callback cancellation, and advancing fake random state;
- binary proof that the deterministic platform marker exists in its test executable but is absent from the production image;
- symbol inspection proving that the deterministic wait test does not import `getchar` or `kbd_poll_key`;
- rejection of a generated C99-only application probe;
- rejection of an unfinished mutation, cyclic FAT, impossible file sizes, and reserved directory names by the image checker;
- rejection of `.`, `..`, and slash-containing injector target names without modifying the image;
- rejection of hard-linked image targets whose aliases could not be atomically replaced together;
- a successful host injection transaction followed by deterministic failure before and after every sector-write and flush stage, with every failed run leaving the original image byte-identical, valid, and free of temporary files;
- deterministic failure of the final permission, flush, close, integrity-check, and rename stages under the same unchanged-original contract;
- safe injector rejection of an unfinished filesystem;
- pre-rename rejection of unrelated corruption that the injection path itself does not traverse, again preserving the original bytes;
- exact-boundary acceptance and one-over-boundary rejection for configurable `elf2bin` object, global-symbol, section, relocation, and output capacities;
- rejection of truncated ELF, invalid section offsets, undefined strong symbols, duplicate strong symbols, unsupported allocatable sections, unsupported relocations, and output overflow by `elf2bin`;
- acceptance of undefined weak symbols with the ELF-defined zero value, plus symbol names and object paths longer than 127 bytes;
- rejection of constructor, destructor, and thread-local-storage sections by both `elf2bin` and the `ld.lld` application layout;
- acceptance and real QEMU execution of exact 512 KiB application images produced independently by `ld.lld` and `elf2bin`;
- QEMU execution of a BSS probe after its final on-disk block padding is deliberately filled with nonzero bytes;
- rejection of 512 KiB plus one byte, overflowing BSS placement, and unresolved-strong-symbol layouts;
- proof that ordinary applications acquire no network objects, network applications acquire the modern-C network objects and compiler helpers, and only the SSH application acquires SSH objects;
- a no-op incremental build;
- runtime-header and kernel-include dependency rebuilding;
- a clean build with `ld.lld` unavailable, forcing `elf2bin`;
- QEMU BSS and application-stack smoke boots for both flat-binary producers.

Temporary repositories, images, logs, and debug kernels are removed when the test finishes.

No source timestamps or tracked files in the working tree are changed.
