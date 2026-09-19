# MINI-OS System Call ABI

Applications are trusted Ring 0 code in the kernel's flat address space.

The `int 0x80` interface is an ABI convention, not a privilege or isolation boundary.

Pointer arguments are trusted and are not copied through a protected user-memory boundary, although individual calls still reject null pointers and invalid lengths where their own contracts require it.

The `int 0x80` descriptor is a 32-bit trap gate, so an already enabled IRQ0 remains deliverable during a system call. Hardware IRQ descriptors remain interrupt gates and therefore keep maskable IRQ handlers non-nested.

## Register Convention

- `EAX`: syscall number on entry and return value on return;
- `EBX`, `ECX`, `EDX`: arguments 1, 2, and 3;
- all general-purpose registers other than `EAX` are preserved;
- the kernel clears DF before any string operation, while an ordinary `iret` restores the caller's saved EFLAGS;
- negative integer returns are errors unless a call documents another form;
- syscall numbers, flags, and common errors are defined once in `transport/lib/syscall.def`.

## Calls

| EAX | Name | EBX | ECX | EDX | Return |
| ---: | --- | --- | --- | --- | --- |
| 1 | `exit` | status (currently ignored) | — | — | Does not return; closes app FDs, resets heap, restores Shell stack |
| 3 | `read` | fd, only 0 | buffer | maximum bytes | line-input byte count; `-2` for bad fd |
| 4 | `write` | fd 1 or 2 | buffer | byte count | count; `-2` for bad fd |
| 5 | `open` | path | open flags | — | fd 3..15 or negative error |
| 6 | `close` | fd | — | — | 0 or `-2` |
| 7 | `getkey` | — | — | — | next supported key/control byte |
| 12 | `brk` | requested break, or 0 to query | — | — | actual break; invalid requests leave it unchanged |
| 14 | `read_file` | fd | buffer | byte count | bytes read, 0 at EOF, or negative error |
| 15 | `write_file` | fd | buffer | byte count | bytes written or negative error |
| 19 | `lseek` | fd | signed offset | `SEEK_SET/CUR/END` | new absolute position or negative error |
| 20 | `move_cursor` | row | column | — | 0; clamps to screen and synchronizes hardware cursor |
| 21 | `clear_screen` | — | — | — | 0 |
| 22 | `set_cursor` | row | column | — | 0; updates logical cursor without immediate hardware sync |
| 23 | `save_screen` | — | — | — | 0; saves 4,000 VGA bytes and cursor position |
| 24 | `restore_screen` | — | — | — | 0; restores saved VGA state and cursor |
| 25 | `get_cursor` | — | — | — | `row * 80 + column` |
| 26 | `clock_monotonic_ms` | — | — | — | low unsigned 32 bits of monotonic milliseconds |
| 27 | `kbd_poll_key` | — | — | — | 0 if no translated key is pending, otherwise a positive key code |
| 28 | `get_random` | destination | byte count | — | exact byte count, or a negative error after failure |
| 29 | `net_get_info` | `struct net_device_info *` | — | — | 0 after a complete snapshot, or `-1` for null |
| 30 | `net_send_frame` | normalized frame | length | — | exact length, or a negative error |
| 31 | `net_recv_frame` | destination | capacity | — | 0 for no frame, positive length, or a negative error |

`read` blocks until input is available, echoes accepted characters, handles backspace, and does not place the terminating newline in the destination. `getkey` returns the driver's translated Set 1 key value, including the control codes used by `vedit`.

`brk` starts at `0x00180000`, accepts values through the exclusive heap end `0x001C0000`, and resets to the start when an application exits.

`clock_monotonic_ms` wraps modulo 2^32 and is intended for unsigned elapsed-time subtraction rather than direct absolute ordering.

`kbd_poll_key` consumes at most one pending Set 1 controller byte, updates keyboard modifier state when necessary, and never waits for input.

`get_random` accepts a zero-length request without inspecting the pointer, rejects nonzero null requests and lengths above 1,024, and has no weak fallback. It uses RDRAND only after CPUID feature detection, retries each 32-bit sample at most ten times, fills the entire request before reporting success, and clears the complete requested destination before returning `SYS_ERR_UNAVAILABLE` after an unavailable source or exhausted retry budget.

`net_get_info` clears and fills the complete versioned structure declared by `transport/lib/net/raw.h`, including state, flags, resources, frame bounds, MAC address, and stable counters, even when the optional device is unavailable.

`net_send_frame` accepts only nonnull normalized frames from 60 through 1,514 bytes, copies the caller data before device access, waits synchronously for completion or one bounded reset attempt, and retains no caller pointer after return.

`net_recv_frame` accepts a nonnull destination and capacity no greater than 1,514 bytes, performs one nonblocking ring poll, removes the four-byte FCS adjustment from a validated NIC count, and consumes a pending over-capacity frame before returning `SYS_ERR_RANGE`.

The raw-frame syscalls use a polling NE2000 device at I/O base `0x300`, keep IRQ9 masked, and are documented in [`Network_Raw_Transport.md`](Network_Raw_Transport.md).

## Open Flags and File Descriptors

Flags may be combined subject to these rules:

| Flag | Value | Meaning |
| --- | ---: | --- |
| `SYS_OPEN_READ` | `0x01` | permit reads |
| `SYS_OPEN_WRITE` | `0x02` | permit writes |
| `SYS_OPEN_CREATE` | `0x04` | create when missing |
| `SYS_OPEN_TRUNCATE` | `0x08` | truncate; requires write and conflicts with append |
| `SYS_OPEN_APPEND` | `0x10` | every write starts at current EOF; requires write |

Descriptors 0, 1, and 2 are console input, console output, and console error.

The application file table has 13 slots, numbered 3 through 15, and is cleared on `exit`.

## Common Errors

| Value | Name | Meaning |
| ---: | --- | --- |
| -1 | `SYS_ERR_INVALID` | invalid syscall, argument, flag set, path, or seek origin |
| -2 | `SYS_ERR_BAD_FD` | invalid, closed, or unavailable descriptor |
| -3 | `SYS_ERR_ACCESS` | descriptor mode rejects the operation |
| -4 | `SYS_ERR_IO` | filesystem metadata or ATA operation failed |
| -5 | `SYS_ERR_RANGE` | a position, length, or capacity is outside the operation's supported range |
| -6 | `SYS_ERR_UNAVAILABLE` | required platform capability or device is unavailable |
| -7 | `SYS_ERR_TIMEOUT` | bounded network DMA or transmission wait expired after successful recovery |
| -8 | `SYS_ERR_DEVICE` | device-reported failure, malformed ring state, or a stable fatal driver |
| -9 | `SYS_ERR_RESET` | network recovery reset or reconfiguration failed |
| -10 | `SYS_ERR_OVERRUN` | receive overrun was detected and the ring was reset successfully |

The stream functions in `minilibc` translate these calls into `FILE` state.

See `docs/Library_Support.md` for that higher-level contract.
