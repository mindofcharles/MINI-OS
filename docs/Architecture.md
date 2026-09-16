# MINI-OS Architecture Notes

## Layered View

### Layer 0: Bootloader

- File: `OS_src/boot/boot.asm`
- Runs in 16-bit real mode.
- Probes BIOS EDD support and loads the kernel one sector at a time with three attempts per sector and disk resets between failed attempts.
- Falls back to BIOS CHS reads when EDD is unavailable or its read path fails.
- Advances the destination segment by 512 bytes per request, so no BIOS transfer buffer crosses a 64 KiB segment boundary.
- Requires firmware-reported conventional RAM through `0x00095000` and contiguous extended RAM through `0x001CB000` before loading the kernel or using the protected-mode fixed regions.
- Enables the fast A20 gate and verifies that physical addresses one MiB apart no longer alias before entering protected mode.
- Initializes temporary execution environment (segments/stack/GDT).
- Carries the host-generated image identity in immutable boot-sector bytes.
- Performs protected-mode transition.

### Layer 1: Kernel Core, Interrupts, IDT Syscalls, and Shell

- File: `OS_src/kernel/main.asm`
- Initializes the console, the complete IDT, the secure-random capability probe, the remapped PIC, the dedicated interrupt-stack path, and the PIT before enabling maskable interrupts.
- Verifies that the primary-master ATA target matches the BIOS-loaded image, then requires a clean, valid filesystem.
- Enters perpetual REPL shell loop.

- File: `OS_src/kernel/idt.asm`
- Manages 256-entry IDT table at physical memory `0x00026000`.
- Installs fatal defaults for every vector, normalized processor-exception entries for vectors 0 through 31, remapped hardware-IRQ entries for vectors `0x20..0x2F`, and the `int 0x80` trap gate.
- Implements the system call handler for console I/O, heap control, filesystem streams, cursor services, monotonic time, nonblocking keyboard polling, and secure random bytes. Calls 1, 3--7, 12, 14, 15, and 19--28 are implemented. The complete register, return-value, flag, and error contract is in [`Syscall_ABI.md`](Syscall_ABI.md).

- File: `OS_src/kernel/interrupts.asm`
- Normalizes exception frames with and without hardware error codes and reports fatal vector/error diagnostics.
- Remaps the master and slave 8259 PICs to vectors `0x20` and `0x28`, masks every line except IRQ0, uses one EOI path, and runs non-nested IRQ handlers on the dedicated interrupt stack.
- Performs an interrupt-disabled startup self-test covering IRQ0, a masked master IRQ, a masked slave IRQ, exact master/slave EOI counts, complete interrupted-context restoration, and canary preservation.

- File: `OS_src/kernel/timer.asm`
- Programs PIT channel 0 in rate-generator mode with divisor 1,193 and advances a wrapping 32-bit millisecond counter on IRQ0.

- File: `OS_src/kernel/random.asm`
- Detects CPUID before testing the RDRAND feature bit and implements bounded all-or-nothing random fills with no fallback source.

- File: `OS_src/kernel/shell.asm`
- Tokenizes command line (`cmd arg1 arg2`).
- Dispatches operations to filesystem wrappers and executable loader (`run`).
- Implements `shell_run`: validates and follows an executable FAT chain, loads a maximum 512 KiB image at `0x00100000`, switches to the bounded application stack ending at `0x001CB000`, and executes it.
- Converts error codes into user-facing messages.

### Layer 2: Drivers

- File: `OS_src/kernel/drivers.asm`
- ATA PIO sector read/write (`LBA28`, primary-channel master only) with bounded readiness waits and `ERR`/`DF` propagation.
- Blocking and nonblocking IBM PC/AT Set 1 keyboard polling using a shared US-layout translator.
- VGA text-mode rendering and cursor control.

### Layer 3: Storage and Utilities

- File: `OS_src/kernel/fs/*.asm`

  Implements metadata lifecycle, path handling, directory mutation, and inode/block allocation.

- File: `OS_src/kernel/utils.asm`

  Shared low-level primitives: zero/copy/string/compare helpers plus memory-region initialization and canary verification.

- File: `OS_src/kernel/platform_layout.def`

  Single source of truth for boot, kernel, buffer, image, heap, argument, stack, canary, configured-memory, and firmware-required-memory constants.

- File: `tools/inject_transport.c`

  Host-side C tool that parses MINI-OS filesystem structures and injects the host `transport/` tree at `/transport/` during `make`.

  It performs all changes on a same-directory temporary copy and exposes them with a final atomic rename.

- File: `tools/check_image.c`

  Read-only final-image verifier for inode reachability, directory consistency, FAT chains, allocation ownership, and geometry.

## Data Model

### Inode

- Type: `0=free`, `1=file`, `2=directory`
- Name: fixed-size field (`27` bytes)
- Size: file byte count
- Start block: first FAT data-block index
- Blocks count: exact number of blocks in the FAT chain
- Parent: parent inode index

### Directory Entry

- Child inode index
- Child type
- Child name

Directory entries are stored in data blocks referenced by directory inodes.

## Key Control Paths

### Mount and Explicit Format

- Read primary-master LBA 0 and compare the immutable boot-code prefix and 48-bit image identity with the BIOS-loaded boot sector.
- Compare an immutable kernel-code sample with the primary-master image.
- On any target mismatch, halt without writing.
- Read the superblock and validate its magic, geometry, root index, and clear unfinished-mutation marker.
- Validate the root inode and its reserved FAT entries.
- On invalid, dirty, or unreadable metadata, halt without formatting.
- After a clean mount, allow the user to invoke the explicit `format` command.

### Persistent Mutation

- Persist unfinished-mutation marker `1` before the first operation write.
- Perform the operation and any recoverable rollback steps.
- A failure before any operation write may clear the marker, but any failure after the first successful operation write keeps it set.
- Persist marker `0` last only after the complete operation, including any required rollback, succeeds.
- Reject later writes in the same boot after any ATA read or write failure.
- Keep the marker set after an I/O failure during a mutation so that the next boot refuses the image.

The marker detects an ambiguous result, but it does not guarantee that earlier sector writes were rolled back.

### Executable Run (`run <file>`)

- Resolve the file inode through path lookup.
- Verify that the target is a regular file (`type == 1`).
- Require a nonzero byte size no greater than 512 KiB and an exact `ceil(size / 512)` block count.
- Validate the complete FAT chain, including range, cycle, and final-EOC checks, before changing the application image.
- Clear `0x00100000..0x0017FFFF`, then follow the FAT chain and read each data block into `0x00100000 + i * 512`.
- Clear the application heap, argument block, and stack, then install adjacent heap and stack canaries before execution.
- Copy bounded argument strings and build `argv` in `0x001C1000..0x001C1FFF`.
- Save the shell stack pointer in `[saved_kernel_esp]`.
- Set `esp = 0x001CB000` and call `0x00100000`.
- On `sys_exit` (`int 0x80`, `eax=1`), restore `[saved_kernel_esp]` in `syscall_entry` and jump to `return_to_shell`.
- Before returning to the prompt, verify the kernel-stack, interrupt-stack, heap, and application-stack canaries and halt on corruption.

Applications are trusted Ring 0 code in the kernel's flat address space.

The syscall ABI organizes application access to kernel services, but it does not provide privilege or memory isolation.

### Interrupt Delivery and Time

Processor exceptions enter normalized fatal handling with a vector and error-code pair, including an inserted zero for exceptions that do not receive a hardware error code.

Every hardware IRQ first saves general-purpose and segment registers on the interrupted stack, installs the known flat data selector, switches to `0x00095000` as the top of the dedicated interrupt stack, dispatches with IF clear, sends EOI through the single PIC path, restores the original stack and registers, and returns with `iretd`.

Only PIT IRQ0 is unmasked. The handler increments a 32-bit counter once per approximately one-millisecond period and sends one master EOI, while the common slave path sends one slave EOI followed by one master EOI.

The syscall descriptor is a trap gate so timer IRQs can interrupt trusted application system calls, including ATA-backed file reads. Other IRQ gates remain interrupt gates, and the common entry treats attempted maskable nesting as fatal.

Secure random requests are independent of the timer. They use CPUID-qualified RDRAND with a ten-attempt bound for every 32-bit word, return only complete fills, and clear the requested destination on source failure.

### Path Resolution

- Choose the root or current working directory as the starting point according to whether the path is absolute or relative.
- Split the path by `/`.
- Handle `.` and `..`.
- Resolve each component through directory lookup.

### Rename/Move

- Split the old and new paths into `(parent, name)` pairs.
- Verify that the destination does not already exist.
- Reject moving a directory into its own subtree.
- Write the destination entry.
- Update the inode parent and name.
- Clear the source entry.

## Memory Map and Static Buffers

The checked physical map, firmware gates, initialization lifecycle, guard behavior, and current isolation limits are defined in [`Memory_Layout.md`](Memory_Layout.md).

The QEMU configuration remains 4 MiB, while the bootloader separately verifies only the conventional and extended spans that the current fixed allocations actually touch.

## Error Strategy

Most filesystem APIs return integer status codes (`0` success, negative error). The shell maps these to messages.

Representative filesystem errors are `-1` not found, `-2` already exists, `-3` not a directory, `-4` directory not empty, `-5` invalid input, `-8` I/O, `-9` corrupt metadata, `-10` protected path, `-11` path too long, and `-12` wrong storage target.

Any ATA I/O error poisons filesystem writes for that boot, and an active mutation also preserves the on-disk marker.
