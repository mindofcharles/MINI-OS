# MINI-OS Memory Layout

This document defines the current physical-memory contract for the whole system.

The authoritative numeric definitions are in `OS_src/kernel/platform_layout.def`, and the bootloader, kernel, Makefile, application linker, host layout checker, and automated tests consume those definitions instead of maintaining separate addresses.

## Address and Firmware Contract

All ranges below are half-open: the start address is included and the end address is excluded.

`PLATFORM_CONFIGURED_MEMORY_BYTES` is currently 4 MiB and controls the default QEMU allocation and the build-time layout ceiling.

This configured capacity is deliberately distinct from the amount reported by legacy BIOS memory services.

Before loading the kernel, the boot sector performs both of these checks:

- BIOS `INT 12h` must report conventional memory through `0x00095000`, which is 596 KiB from physical address zero.
- BIOS `INT 15h/AH=88h` must report at least 812 KiB of contiguous extended memory beginning at `0x00100000`, which covers the system through `0x001CB000`.

The second check covers the highest byte MINI-OS currently allocates rather than trying to prove the complete 4 MiB QEMU configuration with a legacy BIOS interface that may report a smaller compatibility value.

If either check fails, the boot sector prints `M` and halts before loading the kernel or using any protected-mode fixed region that the firmware did not confirm.

After loading the kernel, the boot sector enables A20, saves and modifies the bytes at `0x00000500` and `0x00100500`, tests whether the addresses alias, and restores both original bytes; if they still alias, it prints `A` and halts before protected mode.

## Fixed Physical Layout

- `0x00000500..0x00000501` is temporary low A20-test storage, and its original byte is restored before the kernel starts.
- `0x00007000..0x00007C00` is the real-mode boot stack.
- `0x00007C00..0x00007E00` is the 512-byte boot image.
- `0x00008000..0x00014800` is the 100-sector kernel image reservation.
- `0x00020000..0x00026000` contains six independent 4 KiB kernel work buffers.
- `0x00026000..0x00026800` is the complete 256-entry IDT.
- `0x00027000..0x00028000` is the reserved receive-frame buffer, including private space for word-wide Remote DMA alignment.
- `0x00028000..0x00029000` is the reserved transmit-frame buffer, including private space for word-wide Remote DMA alignment.
- `0x0007F000..0x00080000` is the kernel-stack canary.
- `0x00080000..0x00090000` is the 64 KiB downward-growing kernel stack.
- `0x00090000..0x00091000` is the interrupt-stack canary.
- `0x00091000..0x00095000` is the 16 KiB downward-growing interrupt stack reservation.
- `0x000A0000..0x00100000` is the legacy-memory hole containing display and firmware apertures, not general RAM.
- `0x00100000..0x00180000` is the 512 KiB application image, and `0x00100500` is reused only by the earlier temporary A20 test.
- `0x00180000..0x001C0000` is the 256 KiB application heap.
- `0x001C0000..0x001C1000` is the heap canary.
- `0x001C1000..0x001C2000` is the bounded application argument block.
- `0x001C2000..0x001C3000` is the application-stack canary.
- `0x001C3000..0x001CB000` is the 32 KiB downward-growing application stack.
- `0x001CB000..0x00400000` lies below the configured QEMU ceiling but is neither allocated nor part of the firmware-confirmed requirement, so it must not be treated as an implicit heap or stack.

The gaps between listed ranges are unallocated and must not be used without adding an explicit owner to the shared layout.

## Initialization and Lifetime

Before its first call, the kernel initializes the lower kernel-stack canary and clears the complete 64 KiB kernel stack while no return address or live frame exists.

The kernel then clears the retired real-mode boot stack, the unused tail of its 100-sector reservation, all six work buffers, both frame buffers, the complete interrupt-stack reservation, and the complete application image during early initialization.

The IDT initializer separately clears its full 2 KiB region before installing present fatal defaults, processor-exception gates, remapped PIC IRQ gates, and the system-call trap gate.

The remaining canary pages are filled with byte value `0xA5`, after which the kernel exercises the lower boundary of the interrupt stack while interrupts are disabled, restores the test word to zero, and verifies all canaries before initializing interrupt delivery.

The PIC startup self-test then enters the common IRQ path for IRQ0, masked IRQ1, and masked slave IRQ8 while IF remains clear. Each entry saves its original context, switches to the dedicated stack, returns to an intact sentinel on the kernel stack, and leaves every canary valid before the PIT is programmed and interrupts are enabled.

Before every application run, the loader clears the complete application image, heap, argument block, and stack, then recreates the heap and application-stack canaries.

After loading the executable sectors, the loader clears from the exact logical file end through the end of the image so nonzero padding in the final disk sector cannot contaminate BSS.

The application heap break starts at `0x00180000`, may advance only through `0x001C0000`, and is reset whenever an application exits.

Argument strings are capacity-checked before the executable image is changed, and the resulting strings and `argv` array must remain inside the dedicated 4 KiB argument block.

The kernel verifies all four canaries after application exit and again before every shell prompt; any mismatch prints `MINI_OS: platform memory guard failed; system halted.` and stops the system.

## Build and Runtime Enforcement

`tools/check_layout.c` rejects empty, reversed, misaligned, inconsistent, overlapping, or out-of-contract ranges before the kernel or image is built.

It also checks the BIOS boot address, boot-stack adjacency, kernel sector-counter limit, IDT size, A20 scratch addresses, firmware-visible low and high endpoints, argument containment, the shared maximum raw-frame size plus its private alignment byte, and canary adjacency.

The boot assembly independently rejects an unrepresentable memory requirement, an oversized direct kernel-sector count, or an invalid A20 test layout at assembly time.

The application linker and fallback `elf2bin` producer both enforce the same 512 KiB image range, including non-file-backed BSS placement.

The automated suite boots a deliberately undersized 1 MiB machine and requires the `M` failure marker, runs a strict-C90 program that actively uses 28 KiB of the application stack, and separately corrupts each of the four canary regions in isolated boots to prove every fail-stop path.

## Current Boundaries

MINI-OS uses a fixed physical layout and does not yet parse a full BIOS E820 map, allocate physical pages, enable paging, or isolate applications from the kernel.

The legacy BIOS checks establish the two contiguous RAM spans actually required by the fixed layout, but they do not describe holes or ownership in otherwise unused memory.

Applications are trusted Ring 0 code, so canaries detect selected boundary corruption after control returns but cannot prevent arbitrary writes or recover from corruption.

The interrupt stack is active system memory used only by the non-nested common hardware-IRQ path. System calls continue to use the trusted application's bounded stack, and processor exceptions switch to the known interrupt-stack top only for non-returning fatal diagnostics.
