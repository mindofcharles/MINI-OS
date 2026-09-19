# MINI-OS Raw Ethernet Transport

## Scope

MINI-OS currently provides a polling NE2000-compatible driver and three system calls for complete untagged Ethernet frames.

This transport is the hardware boundary for future modern-C protocol code, but it does not implement Ethernet protocol dispatch, ARP, IPv4, ICMP, UDP, TCP, DHCP, DNS, or SSH.

## Device Contract

The supported QEMU configuration uses an ISA `ne2k_isa` device with these fixed resources:

- I/O base `0x300`.

- IRQ9, configured on the device but masked in the NIC and both 8259 controllers.

- MAC address `52:54:00:12:34:56` in the standard `make run-network` target.

- Six 256-byte transmit pages at NE2000 packet-memory pages `0x40..0x45`.

- A receive ring at pages `0x46..0x7F`, with `BNRY` initially at `0x46` and `CURR` initially at `0x47`.

The driver probes the reset latch, waits for the reset-status bit with a bounded deadline, reads the duplicated word-wide PROM, requires the NE2000 `0x57,0x57` identification bytes, rejects invalid or multicast station addresses, programs the receive ring, and reads the station address and current page back before declaring the device ready.

Failure to identify a device does not prevent the filesystem or shell from starting, and the driver stops and masks the addressed controller before remaining in the stable unavailable state.

## Normalized Frame Contract

The C-facing frame begins at the destination MAC address and excludes the preamble, start-frame delimiter, and frame check sequence.

- The minimum normalized frame length is 60 bytes.

- The maximum normalized frame length is 1,514 bytes.

- The IPv4 MTU reported by the driver is 1,500 bytes.

- Only complete untagged frames are currently supported.

- Code above the driver must zero-pad a short Ethernet payload before calling `net_send_frame`.

NE2000 receive counts include four FCS bytes, so the driver validates the reported count and subtracts four without copying those bytes into the C-facing buffer.

QEMU's NE2000 model reports the same four-byte adjustment in the ring header while copying only the normalized frame data, which is why the driver reads exactly the adjusted length.

Odd frame lengths use one private alignment byte in the fixed kernel frame buffers because word-wide Remote DMA and RBCR are rounded to the next even byte, while the validated receive length and transmit TBCR retain the exact logical frame length.

Every Remote Write is preceded by a completed two-byte dummy Remote Read from packet RAM, as required by the DP8390D PRQ sequencing contract.

## Public C Interface

The strict-C90-compatible declarations and information structure are in `transport/lib/net/raw.h`, while `transport/lib/net/raw.def` is the shared C and assembly source of truth for lengths, states, flags, structure offsets, and structure size.

`net_get_info(struct net_device_info *info)` clears and fills the complete versioned information structure, returning zero on success or `SYS_ERR_INVALID` for a null pointer.

The information snapshot contains device state, driver flags, configured resources, MTU, frame bounds, MAC address, successful frame counts, malformed-length count, receive-capacity drops, overruns, DMA, transmit, and reset timeouts, reset attempts, and fatal transitions.

`net_send_frame(const void *frame, unsigned int length)` validates the pointer and 60-through-1,514-byte range, copies the caller data into the fixed kernel transmit buffer, transfers it to NE2000 packet memory, waits for both Remote DMA and transmit completion, and returns the exact length on success.

The kernel retains no caller pointer after the syscall returns, including all timeout and error paths.

`net_recv_frame(void *frame, unsigned int capacity)` performs one nonblocking poll and returns zero when the ring is empty, a positive normalized frame length on success, or a negative error.

When a validated pending frame exceeds `capacity`, the driver consumes the frame, advances `BNRY`, increments the capacity-drop counter, and returns `SYS_ERR_RANGE`, which prevents one large frame from permanently blocking the ring.

## Receive Validation

The driver reads the four-byte NE2000 ring header before reading frame data and validates all of the following values:

- Packet-received status is present and receive error bits are absent.

- The next-page pointer is inside the configured receive ring.

- The byte count becomes 60 through 1,514 bytes after the four-byte FCS adjustment.

- The next-page pointer equals the page derived from the validated count and four-byte ring header, including ring wrap.

A malformed header is never used as a DMA length or boundary pointer, and the driver discards uncertain ring state through a complete bounded reset and reconfiguration.

Remote DMA reads inherit the programmed NE2000 ring wrap, so a frame split across page `0x7F` and page `0x46` is returned as one contiguous normalized frame in the fixed kernel receive buffer.

## Timeouts and Recovery

Every reset, Remote DMA operation, and transmission has both a monotonic-time deadline and a finite iteration bound.

A DMA or transmit timeout attempts one complete device reset, PROM validation, and ring reconfiguration before returning.

Successful recovery leaves the driver ready but still reports the operation as `SYS_ERR_TIMEOUT`, while failed recovery stops and masks the controller, returns `SYS_ERR_RESET`, moves the driver into the stable fatal state, and makes later frame operations return `SYS_ERR_DEVICE`.

A receive overrun increments its own counter, resets and reconfigures the ring, and returns `SYS_ERR_OVERRUN` after successful recovery.

Device absence returns `SYS_ERR_UNAVAILABLE`, while an explicit device-reported transmission failure returns `SYS_ERR_DEVICE`.

Counters survive driver recovery and reset only when the system reboots.

## Polling and Interrupt State

The first transport deliberately polls the NE2000 ISR and keeps its IMR at zero.

The master PIC continues to unmask only IRQ0 with mask `0xFE`, the slave remains fully masked with `0xFF`, and therefore IRQ9 cannot enter the common interrupt path.

The `int 0x80` trap gate leaves maskable interrupts enabled, so PIT IRQ0 continues advancing the monotonic clock during bounded network syscalls.

## Diagnostics and Tests

The strict-C90 `transport/apps/netdiag.c` application reports ABI version, state, MAC address, I/O base, IRQ, MTU, frame bounds, flags, and every stable counter.

Run it under the supported device configuration with:

```text
make run-network
run /transport/build/apps/netdiag.bin
```

`make test-network-abi` checks the public structure size and every shared field offset.

`make test-network-driver` connects QEMU's NE2000 device to a deterministic host Ethernet peer and verifies exact 60-, 61-, and 1,514-byte transmissions, an exact 61-byte reception, immediate caller-buffer reuse, capacity-drop consumption, two receive-ring wraps, unavailable and wrong-base behavior, synthetic overrun recovery, DMA timeout recovery, transmit timeout recovery, an exact post-recovery transmission, stable reset-timeout failure, stable counters, and continued shell operation.

`make test-network-qemu` combines the deterministic packet-socket regression with the canonical user-network QEMU path, and `make test-network` additionally includes the host platform and raw ABI checks.

Packet captures and debug-console logs live in the temporary test directory during successful runs and are copied to `build/test-artifacts/` with a `-failure` suffix only when the Phase C regression fails.

## Implementation References

The controller behavior follows National Semiconductor's [DP8390D/NS32490D Network Interface Controller data sheet, revision A, July 1995](https://media.digikey.com/pdf/Data%20Sheets/Texas%20Instruments%20PDFs/DP8390D,NS32490D.pdf), including command pages, receive-ring headers, Remote DMA, frame-count handling, and overrun status.

The supported emulator behavior and ISA board interface were validated against QEMU 11.1.1 [`hw/net/ne2000.c`](https://gitlab.com/qemu-project/qemu/-/blob/v11.1.1/hw/net/ne2000.c) and [`hw/net/ne2000-isa.c`](https://gitlab.com/qemu-project/qemu/-/blob/v11.1.1/hw/net/ne2000-isa.c).

QEMU-specific behavior is a compatibility target and must not be treated as proof that untested physical NE2000 boards behave identically.
