# MINI-OS

Currently, MINI-OS is a very small x86 32-bit pure assembly operating system.

MINI-OS supports running strict C90 programs with dynamic memory allocation and a documented subset of familiar C library APIs. Floating-point operations are not supported. The runtime implementation itself uses modern C.

> [!NOTE]
> MINI-OS is only an experimental system and is far from perfect.

Thanks to Gemini, Gork, GPT, and Mistral for their support.

The documentation and some comments were written by Gemini and GPT. A small part of the code was developed in collaboration with Gemini and GPT.

![](/docs/image/OS-demo.png)

(Use QEMU)

👉 [Flowchart](docs/Flowchart.md)

## Current Features

- BIOS boot loader with firmware memory checks, EDD probing, per-sector retries, CHS fallback, and verified A20 enablement before high memory is used
- Protected-mode kernel image at `0x8000`, beginning with an executable entry jump to `kernel_start`
- VGA text console with blocking and nonblocking polling keyboard input
- checked ATA PIO disk I/O (`LBA28`, primary-channel master sector read/write) with boot-image identity verification before mount
- Custom filesystem with persistent directory tree and fail-stop detection of interrupted mutations
- Complete IDT with fatal exception diagnostics, remapped 8259 IRQs, a dedicated interrupt stack, and PIT IRQ0 monotonic timekeeping
- `int 0x80` System Call Engine for console, heap, file, cursor, monotonic-clock, nonblocking-key, and RDRAND-backed secure-random services
- FAT-chain executable loader (`run <file>`) for flat binaries up to 512 KiB at `0x00100000`
- Modern-C runtime implementation with **Dynamic Memory Allocation (`malloc`/`free`/`realloc`/`calloc`)**
- Tested API subset exposed through `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<ctype.h>`, `<limits.h>`, `<stddef.h>`, and `<assert.h>`
- Host-side transactional disk transport tool (`tools/inject_transport.c`) for injecting `/transport/` files without exposing a partial output image
- Built-in shell commands: `help`, `ls`, `pwd`, `cd`, `mkdir`, `touch`, `cat`, `edit`, `rm`, `mv`, `run <file>`, `format`

## Project Layout

- `OS_src/boot/`: bootloader sources
- `OS_src/kernel/`: kernel, shell, drivers, filesystem, IDT & syscalls, and utilities
- `tools/`: host build tools (`inject_transport.c`, `elf2bin.c`, `check_image.c`, `check_layout.c`)
- `transport/`: host files injected into `/transport/` on disk image
  - `transport/lib/`: modern-C runtime and compiler helpers, network/SSH implementation directories, `crt0.asm`, and C90-compatible public headers
  - `transport/apps/`: strict C90 applications (`hello.c`, `calc.c`, `guess.c`, `banner.c`, `vedit.c`)
  - `transport/lib_test/`: strict C90 executable tests, including BSS coverage
  - `transport/build/`: compiled flat output binaries (`apps/*.bin`, `lib_test/*.bin`)
- `docs/`: project documentation
- `build/`: generated kernel binaries and disk image

## Requirements

- `nasm`
- `cc` for modern-C host tools
- `clang` for the freestanding modern-C runtime and strict-C90 apps/tests
- `ld.lld` when available; otherwise the built-in `elf2bin` path is used
- `qemu-system-i386`
- Python 3.9 or newer for automated tests
- standard shell tools used by `Makefile` (`awk`, `dd`, `expr`, `find`, `grep`, `mkdir`, `printf`, `rm`, `tr`, `wc`)
- `bash`, `git`, `nm`, and `objdump` for the optional pinned network feasibility build, plus `patch` for its optional host heap probe

## Build and Run

```bash
make clean
make
make run
```

Build artifacts:

- `build/boot.bin`
- `build/kernel.bin`
- `build/inject_transport`
- `build/elf2bin`
- `build/check_image`
- `build/check_layout`
- `build/mini_os.img`

The image target always runs the read-only integrity checker. Run the complete automated suite with `make test`.

## Quick Usage in Shell

Typical flow after boot:

```text
mkdir docs
cd /docs
touch note.txt
edit note.txt
cat note.txt
mv note.txt note2.txt
ls

cd /transport/build/apps
ls
run hello.bin
run calc.bin

```

## Filesystem Snapshot

Sector layout in current implementation:

- `LBA 0`: boot sector, including a per-image 48-bit identity
- `LBA 1..100`: reserved kernel area
- `LBA 101`: superblock, including the unfinished-mutation marker
- `LBA 102`: inode bitmap
- `LBA 103..118`: FAT
- `LBA 119..374`: inode table
- `LBA 375..4470`: 4,096 data blocks

The generated image is exactly 4,471 sectors (2,289,152 bytes).

## Documentation Index

- Project overview: `docs/Project_Overview.md`
- Architecture: `docs/Architecture.md`
- Physical memory layout: `docs/Memory_Layout.md`
- Code structure: `docs/Code_Structure.md`
- Build and run: `docs/Build_and_Run.md`
- Shell and usage: `docs/Shell_and_Usage.md`
- C90 development guide: `docs/C90_Development_Guide.md`
- Filesystem (current implementation): `docs/Filesystem_Current.md`
- Filesystem design draft: `docs/DIY-FS.md`
- Complete system call ABI: `docs/Syscall_ABI.md`
- Runtime support matrix: `docs/Library_Support.md`
- Automated testing: `docs/Testing.md`
- Real hardware boot guide: `docs/Real_Hardware_Guide.md`
- Known limitations: `docs/Limitations_and_Roadmap.md`

## Real Hardware Note

> [!CAUTION]
> MINI-OS uses a small experimental filesystem without journaling or crash recovery. It is not intended for valuable or long-term storage.
>
> Apart from this, MINI-OS does not perform any destructive operations on the machine. Nevertheless, to prevent potential data loss or hardware damage, it is still recommended to run it on a non-critical machine.

This project currently targets BIOS/CSM-style boot flows.

After BIOS loading, filesystem access requires the boot disk to remain exposed as the primary legacy ATA/IDE master.

`make run` configures QEMU accordingly, but many firmware USB paths do not provide this mapping.

Before mounting, the kernel compares immutable boot/kernel bytes and a per-image 48-bit identity with that ATA target and refuses mismatches without writing.

For USB boot on physical machines, read `docs/Real_Hardware_Guide.md` carefully.

Writing images to raw devices can destroy existing data on that device.
