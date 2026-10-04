# MINI-OS on Real Hardware

This guide explains how to boot MINI-OS from a real USB drive and how to debug the common case where QEMU works but a physical machine shows only a blinking cursor.

> [!CAUTION]
> MINI-OS writes the mounted filesystem and has no repair utility.
>
> Use only a dedicated, non-critical test device.
>
> Writing the image to a raw device erases the prior contents of that target.

## 1. Why QEMU Works but Hardware Fails

QEMU is predictable and usually emulates a legacy BIOS + IDE disk path. Real machines vary widely:

- Many systems default to UEFI-only boot.
- USB boot behavior differs by firmware vendor.
- Some firmware does not expose USB media as classic BIOS disk in the same way.
- CSM/Legacy Boot may be disabled.

MINI_OS currently expects a legacy BIOS-style boot flow.

## 2. Hard Requirements

Before testing on physical hardware, ensure all of the following:

- Boot mode is Legacy BIOS or CSM rather than UEFI-only.
- Secure Boot is disabled.
- BIOS `INT 12h` reports at least 596 KiB of conventional memory, and BIOS `INT 15h/AH=88h` reports at least 812 KiB of contiguous extended memory beginning at one MiB.
- The USB drive is selected in the legacy boot menu.
- The image is written to the whole USB device rather than to a partition.
- After boot, firmware and chipset compatibility expose that same medium as the primary legacy ATA/IDE master (`0x1F0..0x1F7`).
- BIOS loading alone does not guarantee this protected-mode mapping.

Before mount, the kernel compares the executing boot-code prefix, a per-image 48-bit identity, and an immutable kernel-code sample with the primary-master ATA image.

If they differ, it prints a wrong-device message and halts without writing.

This normally protects independently generated images.

Images sharing all checked identity/code bytes, including clones whose filesystems later diverge, cannot be distinguished by this mechanism, and independent 48-bit identities also have a small theoretical collision probability.

## 3. Build and Prepare Image

Build from project root:

```bash
make clean
make
```

Output image:

- `build/mini_os.img`

## 4. Write Image to USB (Whole Device)

Warning: these commands destroy existing data on the target USB drive.

> [!CAUTION]
> Pay close attention to the hard drive serial number, otherwise you may irreversibly erase your system drive

### 4.1 macOS

List disks:

```zsh
diskutil list
```

Unmount target disk (example: disk4):

```zsh
diskutil unmountDisk /dev/disk4
```

Write image (raw device is faster):

```zsh
sudo dd if=build/mini_os.img of=/dev/rdisk4 bs=4m status=progress
sync
```

Eject disk:

```zsh
diskutil eject /dev/disk4
```

### 4.2 Linux

Identify device (example: /dev/sdb):

```bash
lsblk
```

Write image:

```bash
sudo dd if=build/mini_os.img of=/dev/sdb bs=4M conv=fsync status=progress
sync
```

Eject disk:

```bash
sudo eject /dev/sdb
```

### 4.3 Windows

Use a raw image writer (for example Rufus in DD mode, or Win32 Disk Imager), and write `build/mini_os.img` to the entire USB device.

## 5. Firmware (BIOS/UEFI) Settings Checklist

In firmware setup:

- Disable Secure Boot.
- Enable CSM/Legacy Boot.
- Prefer the Legacy USB boot path.
- Temporarily disable Fast Boot.
- Put the USB device first in the boot order, or use the one-time boot menu.

If your machine is UEFI-only with no CSM support, this MINI_OS build will not boot directly.

## 6. Expected Boot Behavior

On successful boot you should see text similar to:

- `MINI_OS: booting kernel...`
- `MINI_OS: filesystem detected.`
- prompt like `/ >`

There is no first-boot automatic format.

An invalid, incompatible, or unfinished filesystem prints `MINI_OS: filesystem unavailable; system halted.` and receives no startup writes.

If you only see a blinking cursor, use the troubleshooting section below.

If kernel loading fails after all EDD and CHS attempts, the bootloader prints `F` and halts.

If the firmware does not report both RAM spans required by the fixed physical layout, the bootloader prints `M` and halts before loading the kernel.

If the fast A20 gate cannot make physical addresses one MiB apart remain distinct, the bootloader prints `A` and halts before entering protected mode.

The loader probes EDD and retries one sector at a time. If EDD is unavailable or repeatedly fails, it restarts the complete kernel load through CHS.

A successful BIOS loading stage does not prove that the later ATA driver can address the same device.

## 7. Troubleshooting: Blinking Cursor Only

### 7.1 Most common causes

- The machine booted in UEFI mode while the image expects BIOS mode.
- The USB image was written to a partition instead of the whole disk.
- The wrong target disk was selected during `dd`.
- Firmware silently rejects the USB geometry or boot path.
- BIOS loaded the USB image, but protected mode does not expose it as primary legacy ATA master.
- A readable but different primary disk produces `MINI_OS: boot device does not match primary ATA master; writes refused.`
- An unavailable ATA path reaches the filesystem fatal message after timeout.

### 7.2 Quick diagnosis steps

- Rebuild and rewrite the image from scratch.
- Re-check that the boot mode is Legacy/CSM.
- Try another USB port, preferably a USB 2.0 port if available.
- Try another USB flash drive model.
- Use the one-time boot menu and explicitly select the legacy USB entry.

### 7.3 Add a visible boot-sector heartbeat

If needed, add a single character print at the very top of `OS_src/boot/boot.asm` before disk read. If the character appears, stage-1 boot code is running and failure is later in the flow.

## 8. Real-Hardware Compatibility Notes (Current Project State)

- The boot sector probes INT 13h extensions and otherwise uses a corrected CHS fallback; both paths issue one-sector requests with bounded retries.
- The boot sector checks the exact conventional and contiguous extended RAM spans used by the current fixed layout, but it does not parse a complete E820 memory map.
- Disk I/O in protected mode uses only primary-master ATA PIO ports (`0x1F0..0x1F7`) and does not remap the BIOS boot device.
- The kernel verifies the image identity before mount and refuses mismatches.
- QEMU is launched with the image explicitly attached as IDE index 0.
- Many real USB boot paths do not preserve an equivalent mapping after protected-mode entry.

The repeatable emulator matrix currently records:

The automated rows below passed with QEMU 11.1.0 on 2026-08-24, and the `q35` negative result was observed in the same environment.

| Environment | Result |
| --- | --- |
| QEMU default PC, normal EDD path | shell ready and full persistence suite passes |
| QEMU `isapc`, normal EDD path | shell ready |
| Forced CHS `5/16/63` | shell ready |
| Forced CHS `66/4/17` | shell ready |
| Forced CHS `263/1/17` | shell ready |
| Boot image on secondary master; primary independently mismatches boot code, identity, or kernel code | all three wrong-device halts; SHA-256 of both images unchanged |
| One-shot EIO on FAT LBA 103 | write-disabled state, persistent unfinished marker, remount refused |
| One-shot EIO while reading `README.TXT` | same-boot writes refused; image unchanged and clean; reboot recovers |
| Two-block write with only one free block | partial write detected, persistent unfinished marker, remount refused |
| QEMU `q35` | filesystem unavailable; outside the primary legacy ATA path |

The first nine rows are covered by `tests/qemu_e2e.py`, while the `q35` row records the explicit negative observation made during this review.

Physical machines have not yet been tested, so the table does not claim firmware or USB-controller compatibility.

## 9. Current Compatibility Boundary

At the current stage, this image should be treated as BIOS/CSM-oriented and primary-legacy-ATA-master-oriented.

A target mismatch is safe-fail rather than an alternate-device fallback.

UEFI-only machines without CSM are outside the present supported path.
