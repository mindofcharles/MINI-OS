# MINI-OS Shell and Usage

This document only describes shell behavior and day-to-day usage inside MINI-OS.

## 1. Shell Entry

After boot and filesystem bootstrap, MINI-OS enters a loop:

- Print the current path prompt (`cwd >`).
- Read one command line from the keyboard.
- Parse up to three tokens (`cmd arg1 arg2`).
- Dispatch the command handler.

Main implementation:

- `OS_src/kernel/main.asm`
- `OS_src/kernel/shell.asm`

## 2. Command List

Supported commands:

- `help`
- `ls`
- `pwd`
- `cd <dir>`
- `mkdir <dir>`
- `touch <file>`
- `cat <file>`
- `edit <file>`
- `rm <path>`
- `mv <old> <new>`
- `run <file>`
- `format`

## 3. Command Behavior

### `help`

Prints command summary.

### `ls`

Lists entries in current directory.

Output format is currently:

- `- name (d)` for directory
- `- name (f)` for regular file

### `pwd`

Prints current absolute path.

### `cd <dir>`

Changes current working directory.

- supports absolute and relative paths
- supports `.` and `..`
- fails if target is missing or not a directory

### `mkdir <dir>`

Creates a directory at the target path.

### `touch <file>`

Creates an empty file at the target path.

### `cat <file>`

Prints file content.

The command validates and follows the complete FAT chain, printing only the bytes covered by the inode size.

### `edit <file>`

Enters text input mode for a file.

- `Enter` inserts newline
- `Backspace` deletes one char
- `ESC` saves and exits

If file does not exist, the shell tries to create it first.

This small editor accepts at most 510 bytes.

Saving truncates an existing file to one data block and releases every former tail block.

### `rm <path>`

Removes file or directory entry.

- root directory cannot be removed
- non-empty directory removal is denied

### `mv <old> <new>`

Renames or moves entry.

- supports cross-directory move
- destination must not already exist
- moving a directory into itself/subtree is rejected

### `run <file>`

Loads and executes a raw binary application from disk.

- resolves target file Inode and verifies regular file type (`INODE_TYPE == 1`)
- validates that the byte size, block count, and complete FAT chain agree
- follows the FAT chain and loads at most 512 KiB into `0x00100000..0x0017FFFF`
- clears the complete 512 KiB image region before loading, including space used by the flat binary's zero-initialized data
- clears the 256 KiB application heap, 4 KiB argument block, and 32 KiB application stack before each run
- installs adjacent heap and application-stack canaries and verifies them before returning to the prompt
- rejects an executable path that does not fit its 256-byte argument field and a second token that does not fit its 512-byte field
- saves kernel Shell stack pointer in `[saved_kernel_esp]`
- sets application stack pointer `esp = 0x001CB000`
- jumps to `0x00100000`
- trusted Ring 0 application executes in the kernel address space and invokes services via `int 0x80`; the complete ABI is in `docs/Syscall_ABI.md`
- upon `sys_exit` (`int 0x80`, `eax=1`), kernel restores `[saved_kernel_esp]` and cleanly returns to Shell prompt

### `format`

Explicitly reformats the already mounted MINI-OS filesystem and resets the current working directory to root.

Startup never invokes this command automatically because invalid, dirty, or incompatible metadata causes a read-only halt before the shell is available.

Formatting sets the persistent unfinished-mutation marker first and commits a clean superblock last.

If any I/O fails, later writes in that boot are refused and the next boot rejects the unfinished filesystem.

The command is destructive to all existing MINI-OS files on the mounted image.

## 4. Typical Usage Flow

Example session 1 (Executing C90 application):

```text
cd /transport/build/apps
ls
run hello.bin

```

The strict-C90 `ping.bin` application accepts one numeric IPv4 address, sends four bounded ICMP Echo Requests using static address `10.0.2.15/24` and gateway `10.0.2.2`, and prints each result plus a summary.

With the supported QEMU network device configured, run it from the applications directory as `run ping.bin 10.0.2.2`; the automated network regression checks both raw-frame transport and protocol-level Ping acceptance.

Example session 2 (File manipulation):

```text
mkdir docs
cd docs
touch note.txt
edit note.txt
cat note.txt
mv note.txt note2.txt
ls
```

## 5. Input Notes

The command grammar is exactly:

```text
command [argument1 [argument2]]
```

Spaces separate tokens. Quoting and escaping are not implemented, names cannot contain spaces, and tokens after `argument2` are ignored.

The interactive command line accepts at most 127 visible characters before its terminator.

Consequently `run` supports an executable path and at most one application argument.

The full-screen `vedit` application holds 21 rows of 77 characters and does not scroll.

It rejects a file outside that format instead of silently splitting or discarding its content, and blocks saving that rejected file from the incomplete editor buffer.

Capacity, load, and save messages remain visible on its control row until the next key press. Run `vedit test` to check row padding and final cursor placement.

Keyboard input polls IBM PC/AT Set 1 scan codes and maps them as a US keyboard.

Supported input consists of letters, the number row and its shifted symbols, common US punctuation, Space, Enter, Escape, Backspace/Delete, arrow keys, both Shift keys, and Ctrl+C/Ctrl+S/Ctrl+Q.

Caps Lock, Alt combinations, function keys, the numeric keypad, layout selection, and asynchronous input are not implemented.

Unsupported keys are ignored. A host layout other than US may therefore produce characters different from its printed key labels.

## 6. Application Trust Model

Loaded applications are trusted kernel-level code. They execute in the same Ring 0 address space as the kernel, with no paging, memory protection, or fault isolation.

They can access kernel memory and privileged instructions, and an application fault can stop the whole system. The `int 0x80` interface is an ABI boundary, not a security boundary.

Implementation:

- `OS_src/kernel/drivers.asm`

## 7. User-Facing Error Messages

Shell handlers map internal error codes to text messages, such as:

- `Entry not found.`
- `Entry already exists.`
- `Target is not a directory.`
- `Target is not a regular file.`
- `Directory is not empty.`
- `Invalid path or name.`
- `Invalid move target.`
- `Filesystem I/O failed; filesystem writes are disabled.`

These mappings are defined in:

- `OS_src/kernel/shell.asm`
- message constants in `OS_src/kernel/main.asm`
