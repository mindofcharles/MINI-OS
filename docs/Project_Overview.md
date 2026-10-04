# MINI-OS Project Overview

## Current Functional Slice

The implemented path is end-to-end:

- bootloader -> kernel -> exceptions/PIC/PIT -> filesystem mount and polling NE2000 initialization -> shell -> C90 application loading (`run`) -> IDT/syscalls -> persistent disk and raw-frame operations

## Main Source Areas

- `OS_src/boot/`: real-mode boot code and PM transition path
- `OS_src/kernel/`: shell loop, exceptions, PIC/PIT, secure random, IDT and syscalls, ATA/keyboard/VGA drivers, polling NE2000 transport, and utilities
- `OS_src/kernel/fs/`: filesystem logic
- `tools/`: host C tools (`inject_transport.c`, `elf2bin.c`, `check_image.c`, `check_layout.c`)
- `transport/`: host files injected into `/transport/` (strict-C90 apps and tests, modern-C runtime implementation, and `crt0`)
- `build/`: generated binaries and image
- `docs/`: documentation set

## Where to Read Next

- Architecture: `docs/Architecture.md`
- Physical memory layout: `docs/Memory_Layout.md`
- Build and run: `docs/Build_and_Run.md`
- Shell usage: `docs/Shell_and_Usage.md`
- C90 development guide: `docs/C90_Development_Guide.md`
- Filesystem current implementation: `docs/Filesystem_Current.md`
- Filesystem design idea draft: `docs/DIY-FS.md`
- System call ABI: `docs/Syscall_ABI.md`
- Raw Ethernet transport: `docs/Network_Raw_Transport.md`
- Ethernet, ARP, IPv4, and ICMP Echo: `docs/Network_Stack.md`
- Runtime support matrix: `docs/Library_Support.md`
- Automated tests: `docs/Testing.md`
- Real hardware guide: `docs/Real_Hardware_Guide.md`
- Limitations: `docs/Limitations_and_Roadmap.md`

## Included Applications

| Source | Behavior |
| :--- | :--- |
| `hello.c` | prints a greeting and integer-format examples |
| `calc.c` | interactive integer calculator |
| `guess.c` | asks for a name and prints a greeting |
| `banner.c` | renders text with a 5-by-5 glyph table |
| `vedit.c` | full-screen text editor using file and cursor syscalls |
| `netdiag.c` | reports NE2000 state, configuration, frame bounds, and counters |
| `ping.c` | sends four bounded ICMP Echo Requests to a numeric IPv4 address |

These binaries are trusted Ring 0 programs in the kernel address space; the loader is not a process-isolation boundary.
