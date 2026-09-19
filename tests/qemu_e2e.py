#!/usr/bin/env python3
"""Deterministic QEMU regression for the MINI-OS shell and filesystem."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Optional


BOOT_TIMEOUT = 10.0
COMMAND_TIMEOUT = 12.0
BOOT_ID_OFFSET = 504
BOOT_ID_SIZE = 6
SUPERBLOCK_LBA = 101
INODE_BITMAP_LBA = 102
FAT_LBA = 103
DATA_BLOCK_COUNT = 4096


def platform_layout() -> dict[str, int]:
    definition = Path(__file__).resolve().parents[1] / "OS_src/kernel/platform_layout.def"
    pattern = re.compile(
        r"^PLATFORM_LAYOUT_CONST\(([A-Z0-9_]+),\s*"
        r"(0x[0-9A-Fa-f]+|[0-9]+)\)$"
    )
    values: dict[str, int] = {}
    for line in definition.read_text(encoding="ascii").splitlines():
        match = pattern.fullmatch(line)
        if match is not None:
            values[match.group(1)] = int(match.group(2), 0)
    required = {"PLATFORM_CONFIGURED_MEMORY_BYTES", "KERNEL_IMAGE_MAX_SIZE"}
    missing = required.difference(values)
    if missing:
        raise RuntimeError(
            "platform layout is missing: " + ", ".join(sorted(missing))
        )
    return values


PLATFORM_LAYOUT = platform_layout()
CONFIGURED_MEMORY_BYTES = PLATFORM_LAYOUT["PLATFORM_CONFIGURED_MEMORY_BYTES"]
if CONFIGURED_MEMORY_BYTES == 0 or CONFIGURED_MEMORY_BYTES % (1024 * 1024) != 0:
    raise RuntimeError("configured guest memory must use whole MiB units")
QEMU_MEMORY = f"{CONFIGURED_MEMORY_BYTES // (1024 * 1024)}M"
KERNEL_IMAGE_MAX_SIZE = PLATFORM_LAYOUT["KERNEL_IMAGE_MAX_SIZE"]
if KERNEL_IMAGE_MAX_SIZE == 0 or KERNEL_IMAGE_MAX_SIZE % 512 != 0:
    raise RuntimeError("kernel image reservation must use whole sectors")
KERNEL_MAX_SECTORS = KERNEL_IMAGE_MAX_SIZE // 512


def run_checked(command: list[str], cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def prepare_debug_image(
    repo: Path, source: Path, destination: Path, force_chs: bool = False,
    force_a20_failure: bool = False,
    kernel_defines: Optional[list[str]] = None,
) -> None:
    kernel = destination.with_suffix(".kernel.bin")
    boot = destination.with_suffix(".boot.bin")
    kernel_command = [
        "nasm",
        "-w-label-redef-late",
        "-d",
        "ENABLE_DEBUGCON=1",
    ]
    for definition in kernel_defines or []:
        kernel_command.extend(["-d", definition])
    kernel_command.extend(
        ["-f", "bin", "OS_src/kernel/main.asm", "-o", str(kernel)]
    )
    run_checked(kernel_command, repo)
    kernel_bytes = kernel.read_bytes()
    sectors = (len(kernel_bytes) + 511) // 512
    if sectors > KERNEL_MAX_SECTORS:
        raise RuntimeError(
            f"debug kernel exceeds the {KERNEL_MAX_SECTORS}-sector boot reservation"
        )
    boot_command = [
        "nasm",
        "-w-label-redef-late",
        "-d",
        "ENABLE_DEBUGCON=1",
        "-d",
        f"KERNEL_SECTORS={sectors}",
    ]
    if force_chs:
        boot_command.extend(["-d", "BOOT_FORCE_CHS=1"])
    if force_a20_failure:
        boot_command.extend(["-d", "BOOT_FORCE_A20_FAIL=1"])
    boot_command.extend(["-f", "bin", "OS_src/boot/boot.asm", "-o", str(boot)])
    run_checked(boot_command, repo)
    boot_bytes = bytearray(boot.read_bytes())
    if len(boot_bytes) != 512 or boot_bytes[510:] != b"\x55\xaa":
        raise RuntimeError("debug boot sector has an invalid size or signature")

    with source.open("rb") as source_image:
        source_image.seek(BOOT_ID_OFFSET)
        boot_id = source_image.read(BOOT_ID_SIZE)
    if len(boot_id) != BOOT_ID_SIZE or not any(boot_id):
        raise RuntimeError("source image has no boot volume ID")
    boot_bytes[BOOT_ID_OFFSET : BOOT_ID_OFFSET + BOOT_ID_SIZE] = boot_id

    shutil.copy2(source, destination)
    with destination.open("r+b") as image:
        image.seek(0)
        image.write(boot_bytes)
        image.seek(512)
        image.write(b"\0" * KERNEL_IMAGE_MAX_SIZE)
        image.seek(512)
        image.write(kernel_bytes)


class VirtualMachine:
    def __init__(
        self,
        image: Path,
        log: Path,
        qemu: str,
        instance: int,
        storage_args: Optional[list[str]] = None,
        extra_args: Optional[list[str]] = None,
        memory: Optional[str] = None,
    ) -> None:
        self.image = image
        self.log = log
        self.qemu = qemu
        self.instance = instance
        self.storage_args = storage_args
        self.extra_args = extra_args or []
        self.memory = memory or QEMU_MEMORY
        self.process: Optional[subprocess.Popen[bytes]] = None

    def start(
        self, expected: str = "MINI_OS: shell ready", timeout: float = BOOT_TIMEOUT
    ) -> None:
        self.log.write_bytes(b"")
        storage_args = self.storage_args or [
            "-drive",
            f"file={self.image},format=raw,if=ide,index=0,media=disk",
        ]
        command = [
            self.qemu,
            "-m",
            self.memory,
            *storage_args,
            *self.extra_args,
            "-display",
            "none",
            "-monitor",
            "stdio",
            "-debugcon",
            f"file:{self.log}",
            "-global",
            "isa-debugcon.iobase=0xe9",
            "-no-reboot",
            "-no-shutdown",
        ]
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if self.process.poll() is not None:
            error = self.process.stderr.read().decode("utf-8", "replace")
            raise RuntimeError(f"QEMU exited during startup: {error}")
        self._read_prompt()
        self.wait_for(expected, 0, timeout)

    def _read_prompt(self) -> str:
        if self.process is None or self.process.stdout is None:
            raise RuntimeError("QEMU monitor is not connected")
        response = bytearray()
        deadline = time.monotonic() + 3.0
        while b"(qemu)" not in response:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("timed out reading the QEMU monitor prompt")
            ready, _, _ = select.select([self.process.stdout], [], [], remaining)
            if not ready:
                raise RuntimeError("timed out reading the QEMU monitor prompt")
            chunk = os.read(self.process.stdout.fileno(), 4096)
            if not chunk:
                error = ""
                if self.process.poll() is not None and self.process.stderr is not None:
                    error = self.process.stderr.read().decode("utf-8", "replace")
                raise RuntimeError(f"QEMU monitor closed unexpectedly: {error}")
            response.extend(chunk)
        return response.decode("utf-8", "replace")

    def hmp(self, command: str) -> str:
        if self.process is None or self.process.stdin is None:
            raise RuntimeError("QEMU monitor is not connected")
        self.process.stdin.write(command.encode("ascii") + b"\n")
        self.process.stdin.flush()
        return self._read_prompt()

    @staticmethod
    def key_name(character: str) -> str:
        if character.isascii() and (character.islower() or character.isdigit()):
            return character
        if character.isascii() and character.isupper():
            return "shift-" + character.lower()
        mapping = {
            " ": "spc",
            "/": "slash",
            ".": "dot",
            "_": "shift-minus",
            "-": "minus",
        }
        if character not in mapping:
            raise ValueError(f"no QEMU key mapping for {character!r}")
        return mapping[character]

    def send_command(self, command: str) -> int:
        start = self.log.stat().st_size
        for character in command:
            self.hmp(f"sendkey {self.key_name(character)} 2")
        self.hmp("sendkey ret 2")
        return start

    def text(self) -> str:
        return self.log.read_bytes().decode("latin1", "replace")

    def wait_for(self, expected: str, start: int, timeout: float) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            data = self.text()
            segment = data[start:]
            if expected in segment:
                return segment
            if self.process is not None and self.process.poll() is not None:
                error = self.process.stderr.read().decode("utf-8", "replace")
                raise RuntimeError(f"QEMU exited while waiting for {expected!r}: {error}")
            time.sleep(0.03)
        raise RuntimeError(
            f"timed out waiting for {expected!r}; debug console follows:\n{self.text()}"
        )

    def command_expect(self, command: str, expected: str) -> str:
        start = self.send_command(command)
        return self.wait_for(expected, start, COMMAND_TIMEOUT)

    def stop(self) -> None:
        if self.process is None:
            return
        if self.process.poll() is None:
            try:
                self.hmp("quit")
            except (OSError, RuntimeError):
                self.process.terminate()
        try:
            self.process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5.0)


def check_image(checker: Path, image: Path, repo: Path) -> None:
    run_checked([str(checker), str(image)], repo)


def expect_image_rejected(
    checker: Path, image: Path, repo: Path, expected: str
) -> None:
    result = subprocess.run(
        [str(checker), str(image)], cwd=repo, text=True, capture_output=True
    )
    output = result.stdout + result.stderr
    if result.returncode == 0 or expected not in output:
        raise RuntimeError(
            f"checker did not reject {image.name} with {expected!r}:\n{output}"
        )


def image_digest(image: Path) -> str:
    digest = hashlib.sha256()
    with image.open("rb") as stream:
        while chunk := stream.read(64 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def blockdev(arguments: dict[str, object]) -> list[str]:
    return ["-blockdev", json.dumps(arguments, separators=(",", ":"))]


def count_free_data_blocks(image: Path) -> int:
    with image.open("rb") as stream:
        stream.seek(FAT_LBA * 512)
        fat = stream.read(DATA_BLOCK_COUNT * 2)
    if len(fat) != DATA_BLOCK_COUNT * 2:
        raise RuntimeError("image has a truncated FAT")
    return sum(
        1
        for block in range(2, DATA_BLOCK_COUNT)
        if fat[block * 2 : block * 2 + 2] == b"\0\0"
    )


def small_application_test(vm: VirtualMachine) -> None:
    segment = vm.command_expect(
        "run /transport/build/apps/hello.bin",
        "MINI_OS: app exited cleanly.",
    )
    if (
        "Hello, World from C90 in MINI-OS!" not in segment
        or "Test number: 42, Hex: 0xff" not in segment
    ):
        raise RuntimeError("strict-C90 hello application output changed")


def smoke_test(vm: VirtualMachine, program: Optional[str] = None,
               expected: str = "MINI_OS: app exited cleanly.") -> None:
    small_application_test(vm)
    vm.command_expect(
        "run /transport/build/lib_test/test_bss.bin", "BSS test: PASS"
    )
    vm.command_expect(
        "run /transport/build/lib_test/test_stack.bin", "STACK CANARY TEST: PASS"
    )
    vm.command_expect(
        "run /transport/build/lib_test/test_platform.bin",
        "PLATFORM TIMER TEST: PASS",
    )
    if program is not None:
        vm.command_expect("run " + program, expected)
    vm.command_expect("pwd", "/ > ")


def filesystem_boundary_test(vm: VirtualMachine) -> None:
    name_26 = "abcdefghijklmnopqrstuvwxyz"
    name_27 = "abcdefghijklmnopqrstuvwxyza"

    vm.command_expect("vedit test", "vedit render test: PASS")
    vm.command_expect("touch " + name_26, "Created: " + name_26)
    vm.command_expect("touch " + name_27, "Invalid path or name.")
    vm.command_expect("rm " + name_26, "Removed.")

    vm.command_expect("mkdir many", "Directory created: many")
    vm.command_expect("cd many", "/many > ")
    for index in range(17):
        name = f"f{index:02d}"
        vm.command_expect("touch " + name, "Created: " + name)
    vm.command_expect("mv f16 z16", "Renamed.")
    vm.command_expect("ls", "z16 (f)")
    for index in range(16):
        vm.command_expect(f"rm f{index:02d}", "Removed.")
    vm.command_expect("rm z16", "Removed.")
    vm.command_expect("cd /", "/ > ")
    vm.command_expect("rm many", "Removed.")

    vm.command_expect("mkdir CaseDir", "Directory created: CaseDir")
    vm.command_expect("mv CaseDir cASEdIR", "Renamed.")
    vm.command_expect("cd casedir", "/cASEdIR > ")
    vm.command_expect(
        "rm /casedir", "Cannot remove root, current directory, or its ancestor."
    )
    vm.command_expect("mkdir child", "Directory created: child")
    vm.command_expect("cd child", "/cASEdIR/child > ")
    vm.command_expect(
        "rm /casedir", "Cannot remove root, current directory, or its ancestor."
    )
    vm.command_expect("mv /casedir /Moved", "Renamed.")
    vm.command_expect("pwd", "/Moved/child")
    vm.command_expect("cd /", "/ > ")
    vm.command_expect("rm /Moved/child", "Removed.")
    vm.command_expect("rm /Moved", "Removed.")

    vm.command_expect("touch slashfile", "Created: slashfile")
    vm.command_expect("cat slashfile/", "Target is not a directory.")
    vm.command_expect("rm slashfile/", "Invalid path or name.")
    vm.command_expect(
        "rm /", "Cannot remove root, current directory, or its ancestor."
    )
    vm.command_expect("rm slashfile", "Removed.")


def forced_chs_test(
    repo: Path, checker: Path, source: Path, qemu: str, temp: Path
) -> None:
    image = temp / "mini_os_chs.img"
    prepare_debug_image(repo, source, image, force_chs=True)
    geometries = ((5, 16, 63), (66, 4, 17), (263, 1, 17))
    for index, (cylinders, heads, sectors) in enumerate(geometries):
        storage = [
            *blockdev(
                {
                    "driver": "file",
                    "filename": str(image),
                    "node-name": f"chs-file-{index}",
                }
            ),
            *blockdev(
                {
                    "driver": "raw",
                    "file": f"chs-file-{index}",
                    "node-name": f"chs-raw-{index}",
                }
            ),
            "-device",
            (
                f"ide-hd,drive=chs-raw-{index},bus=ide.0,unit=0,"
                f"cyls={cylinders},heads={heads},secs={sectors}"
            ),
        ]
        vm = VirtualMachine(
            image, temp / f"session-chs-{index}.log", qemu, -1, storage
        )
        try:
            vm.start()
            vm.command_expect("pwd", "/ > ")
        finally:
            vm.stop()
    check_image(checker, image, repo)


def a20_failure_test(repo: Path, source: Path, qemu: str, temp: Path) -> None:
    image = temp / "mini_os_a20_failure.img"
    prepare_debug_image(repo, source, image, force_a20_failure=True)
    vm = VirtualMachine(image, temp / "session-a20-failure.log", qemu, -20)
    try:
        vm.start("A")
    finally:
        vm.stop()


def insufficient_memory_test(
    repo: Path, source: Path, qemu: str, temp: Path
) -> None:
    image = temp / "mini_os_insufficient_memory.img"
    prepare_debug_image(repo, source, image)
    vm = VirtualMachine(
        image,
        temp / "session-insufficient-memory.log",
        qemu,
        -22,
        memory="1M",
    )
    try:
        vm.start("M")
    finally:
        vm.stop()


def memory_guard_failure_test(
    source: Path, qemu: str, temp: Path
) -> None:
    for index, guard in enumerate(("kernel", "interrupt", "heap", "application")):
        image = temp / f"mini_os_{guard}_guard_failure.img"
        shutil.copy2(source, image)
        vm = VirtualMachine(
            image,
            temp / f"session-{guard}-guard-failure.log",
            qemu,
            -23 - index,
        )
        try:
            vm.start()
            vm.command_expect(
                "run /transport/build/lib_test/test_guard.bin " + guard,
                "MINI_OS: platform memory guard failed; system halted.",
            )
        finally:
            vm.stop()


def network_run_configuration_test(
    repo: Path, source: Path, qemu: str, temp: Path
) -> None:
    expected_command = (
        f"qemu-system-i386 -m {QEMU_MEMORY} -accel tcg "
        "-cpu max,rdrand=on "
        "-drive file=build/mini_os.img,format=raw,if=ide,index=0,media=disk "
        "-netdev user,id=net0 "
        "-device ne2k_isa,netdev=net0,iobase=0x300,irq=9,"
        "mac=52:54:00:12:34:56"
    )
    dry_run = subprocess.run(
        ["make", "-n", "run-network"],
        cwd=repo,
        check=True,
        text=True,
        capture_output=True,
    )
    if expected_command not in dry_run.stdout.splitlines():
        raise RuntimeError("make run-network does not expose the Phase A contract")

    image = temp / "mini_os_network_run.img"
    prepare_debug_image(repo, source, image)
    vm = VirtualMachine(
        image,
        temp / "session-network-run.log",
        qemu,
        -21,
        extra_args=[
            "-accel",
            "tcg",
            "-cpu",
            "max,rdrand=on",
            "-netdev",
            "user,id=net0",
            "-device",
            "ne2k_isa,netdev=net0,iobase=0x300,irq=9,mac=52:54:00:12:34:56",
        ],
    )
    try:
        vm.start()
        vm.command_expect(
            "run /transport/build/lib_test/test_platform.bin random",
            "RANDOM TEST: PASS",
        )
        segment = vm.command_expect(
            "run /transport/build/apps/netdiag.bin",
            "/ > ",
        )
        for expected in (
            "NE2000 state: ready",
            "ABI version: 1",
            "MAC: 52:54:00:12:34:56",
            "I/O base: 0x300",
            "IRQ: 9",
            "MTU: 1500",
            "Frame bounds: 60-1514",
            "Driver flags: 0x1f",
        ):
            if expected not in segment:
                raise RuntimeError(
                    f"network diagnostic omitted {expected!r}:\n{segment}"
                )
        vm.command_expect("pwd", "/ > ")
    finally:
        vm.stop()


def random_unavailable_test(
    repo: Path, source: Path, qemu: str, temp: Path
) -> None:
    cases = (
        ("feature-off", ["-accel", "tcg", "-cpu", "qemu32,rdrand=off"]),
        ("no-cpuid", ["-accel", "tcg", "-cpu", "486"]),
    )
    for index, (name, extra_args) in enumerate(cases):
        image = temp / f"mini_os_random_unavailable_{name}.img"
        prepare_debug_image(repo, source, image)
        vm = VirtualMachine(
            image,
            temp / f"session-random-unavailable-{name}.log",
            qemu,
            -28 - index,
            extra_args=extra_args,
        )
        try:
            vm.start()
            vm.command_expect(
                "run /transport/build/lib_test/test_platform.bin unavailable",
                "RANDOM UNAVAILABLE TEST: PASS",
            )
        finally:
            vm.stop()


def exception_handling_test(
    repo: Path, source: Path, qemu: str, temp: Path
) -> None:
    cases = (
        (
            "no-error",
            "KERNEL_TEST_EXCEPTION_NO_ERROR=1",
            "MINI_OS: fatal exception vector=0x00000000 error=0x00000000; system halted.",
        ),
        (
            "with-error",
            "KERNEL_TEST_EXCEPTION_ERROR=1",
            "MINI_OS: fatal exception vector=0x0000000D error=0x00000018; system halted.",
        ),
    )
    for index, (name, definition, expected) in enumerate(cases):
        image = temp / f"mini_os_exception_{name}.img"
        prepare_debug_image(
            repo, source, image, kernel_defines=[definition]
        )
        vm = VirtualMachine(
            image,
            temp / f"session-exception-{name}.log",
            qemu,
            -29 - index,
        )
        try:
            vm.start(expected)
        finally:
            vm.stop()


def machine_compatibility_test(
    repo: Path, checker: Path, source: Path, qemu: str, temp: Path
) -> None:
    image = temp / "mini_os_isapc.img"
    shutil.copy2(source, image)
    vm = VirtualMachine(
        image,
        temp / "session-isapc.log",
        qemu,
        -2,
        extra_args=["-machine", "isapc"],
    )
    try:
        vm.start()
        vm.command_expect("pwd", "/ > ")
    finally:
        vm.stop()
    check_image(checker, image, repo)


def wrong_device_test(
    repo: Path, checker: Path, source: Path, qemu: str, temp: Path
) -> None:
    with source.open("rb") as stream:
        stream.seek(512)
        entry_stub = stream.read(5)
    if len(entry_stub) == 5 and entry_stub[0] == 0xE9:
        instruction_size = 5
        displacement = int.from_bytes(entry_stub[1:], "little", signed=True)
    elif len(entry_stub) >= 2 and entry_stub[0] == 0xEB:
        instruction_size = 2
        displacement = int.from_bytes(entry_stub[1:2], "little", signed=True)
    else:
        raise RuntimeError("debug kernel has no entry stub")
    kernel_start = instruction_size + displacement
    if (
        kernel_start < instruction_size
        or kernel_start + 10 >= KERNEL_IMAGE_MAX_SIZE
    ):
        raise RuntimeError("debug kernel entry stub has an invalid target")
    cases = (
        ("boot-code", 100, 0x5A),
        ("identity", BOOT_ID_OFFSET, 0xA5),
        ("kernel-code", 512 + kernel_start + 10, 0x3C),
    )

    for index, (label, offset, mask) in enumerate(cases):
        boot_image = temp / f"wrong-device-boot-{label}.img"
        victim_image = temp / f"wrong-device-primary-{label}.img"
        shutil.copy2(source, boot_image)
        shutil.copy2(source, victim_image)
        with victim_image.open("r+b") as stream:
            if label == "identity":
                stream.seek(offset)
                identity = bytearray(stream.read(BOOT_ID_SIZE))
                if len(identity) != BOOT_ID_SIZE:
                    raise RuntimeError("cannot prepare identity mismatch")
                identity[0] ^= mask
                if not any(identity):
                    identity[0] = 1
                stream.seek(offset)
                stream.write(identity)
            else:
                stream.seek(offset)
                original = stream.read(1)
                if len(original) != 1:
                    raise RuntimeError(f"cannot prepare {label} mismatch")
                stream.seek(offset)
                stream.write(bytes((original[0] ^ mask,)))

        before_boot = image_digest(boot_image)
        before_victim = image_digest(victim_image)
        victim_file = f"victim-file-{index}"
        victim_raw = f"victim-raw-{index}"
        boot_file = f"boot-file-{index}"
        boot_raw = f"boot-raw-{index}"
        storage = [
            *blockdev(
                {
                    "driver": "file",
                    "filename": str(victim_image),
                    "node-name": victim_file,
                }
            ),
            *blockdev(
                {"driver": "raw", "file": victim_file, "node-name": victim_raw}
            ),
            "-device",
            f"ide-hd,drive={victim_raw},bus=ide.0,unit=0,bootindex=2",
            *blockdev(
                {
                    "driver": "file",
                    "filename": str(boot_image),
                    "node-name": boot_file,
                }
            ),
            *blockdev(
                {"driver": "raw", "file": boot_file, "node-name": boot_raw}
            ),
            "-device",
            f"ide-hd,drive={boot_raw},bus=ide.1,unit=0,bootindex=1",
        ]
        vm = VirtualMachine(
            boot_image,
            temp / f"session-wrong-device-{label}.log",
            qemu,
            -3 - index,
            storage,
        )
        try:
            vm.start(
                "MINI_OS: boot device does not match primary ATA master; writes refused."
            )
        finally:
            vm.stop()

        if image_digest(boot_image) != before_boot:
            raise RuntimeError(f"{label} mismatch modified the actual boot image")
        if image_digest(victim_image) != before_victim:
            raise RuntimeError(f"{label} mismatch modified the primary-master image")
        check_image(checker, boot_image, repo)
        check_image(checker, victim_image, repo)


def invalid_filesystem_boot_test(source: Path, qemu: str, temp: Path) -> None:
    image = temp / "invalid-filesystem.img"
    shutil.copy2(source, image)
    with image.open("r+b") as stream:
        stream.seek(SUPERBLOCK_LBA * 512)
        stream.write(b"BAD!")
    before = image_digest(image)
    vm = VirtualMachine(image, temp / "session-invalid-filesystem.log", qemu, -4)
    try:
        vm.start("MINI_OS: filesystem unavailable; system halted.")
    finally:
        vm.stop()
    if image_digest(image) != before:
        raise RuntimeError("invalid filesystem was modified during startup")


def explicit_format_test(
    repo: Path, checker: Path, source: Path, qemu: str, temp: Path
) -> None:
    image = temp / "explicit-format.img"
    shutil.copy2(source, image)
    vm = VirtualMachine(image, temp / "session-explicit-format.log", qemu, -5)
    try:
        vm.start()
        vm.command_expect("touch doomed", "Created: doomed")
        vm.command_expect("format", "MINI_OS: format complete.")
        start = vm.send_command("ls")
        segment = vm.wait_for("/ > ", start, COMMAND_TIMEOUT)
        if "README.TXT (f)" not in segment:
            raise RuntimeError("explicit format did not recreate README.TXT")
        if "transport (d)" in segment or "doomed (f)" in segment:
            raise RuntimeError("explicit format retained old filesystem entries")
    finally:
        vm.stop()
    check_image(checker, image, repo)

    reboot = VirtualMachine(image, temp / "session-format-reboot.log", qemu, -6)
    try:
        reboot.start()
        reboot.command_expect("cat README.TXT", "Welcome to MINI_OS.")
    finally:
        reboot.stop()
    check_image(checker, image, repo)


def mutation_fault_test(
    repo: Path, checker: Path, source: Path, qemu: str, temp: Path
) -> None:
    image = temp / "mutation-fault.img"
    config = temp / "mutation-fault.blkdebug"
    shutil.copy2(source, image)
    config.write_text(
        '[inject-error]\n'
        'event = "pwritev"\n'
        'errno = "5"\n'
        'sector = "103"\n'
        'once = "on"\n'
        'immediately = "on"\n',
        encoding="ascii",
    )
    storage = [
        *blockdev({"driver": "file", "filename": str(image), "node-name": "fault-file"}),
        *blockdev(
            {
                "driver": "blkdebug",
                "image": "fault-file",
                "config": str(config),
                "node-name": "fault-debug",
            }
        ),
        *blockdev({"driver": "raw", "file": "fault-debug", "node-name": "fault-raw"}),
        "-device",
        "ide-hd,drive=fault-raw,bus=ide.0,unit=0",
    ]
    vm = VirtualMachine(
        image, temp / "session-mutation-fault.log", qemu, -7, storage
    )
    try:
        vm.start()
        vm.command_expect(
            "rm README.TXT",
            "Filesystem I/O failed; filesystem writes are disabled.",
        )
        vm.command_expect(
            "touch blocked",
            "Filesystem I/O failed; filesystem writes are disabled.",
        )
    finally:
        vm.stop()

    expect_image_rejected(
        checker, image, repo, "filesystem has an unfinished mutation"
    )
    reboot = VirtualMachine(image, temp / "session-mutation-reboot.log", qemu, -8)
    try:
        reboot.start("MINI_OS: filesystem unavailable; system halted.")
    finally:
        reboot.stop()


def read_fault_test(
    repo: Path, checker: Path, source: Path, qemu: str, temp: Path
) -> None:
    image = temp / "read-fault.img"
    config = temp / "read-fault.blkdebug"
    shutil.copy2(source, image)
    before = image_digest(image)
    config.write_text(
        '[inject-error]\n'
        'event = "read_aio"\n'
        'errno = "5"\n'
        'sector = "378"\n'
        'once = "on"\n'
        'immediately = "on"\n',
        encoding="ascii",
    )
    storage = [
        *blockdev({"driver": "file", "filename": str(image), "node-name": "read-file"}),
        *blockdev(
            {
                "driver": "blkdebug",
                "image": "read-file",
                "config": str(config),
                "node-name": "read-debug",
            }
        ),
        *blockdev({"driver": "raw", "file": "read-debug", "node-name": "read-raw"}),
        "-device",
        "ide-hd,drive=read-raw,bus=ide.0,unit=0",
    ]
    vm = VirtualMachine(image, temp / "session-read-fault.log", qemu, -9, storage)
    try:
        vm.start()
        vm.command_expect(
            "cat README.TXT",
            "Filesystem I/O failed; filesystem writes are disabled.",
        )
        vm.command_expect(
            "touch blocked",
            "Filesystem I/O failed; filesystem writes are disabled.",
        )
    finally:
        vm.stop()
    if image_digest(image) != before:
        raise RuntimeError("read failure or blocked mutation changed the image")
    check_image(checker, image, repo)

    reboot = VirtualMachine(image, temp / "session-read-fault-reboot.log", qemu, -10)
    try:
        reboot.start()
        reboot.command_expect("touch recovered", "Created: recovered")
        reboot.command_expect("rm recovered", "Removed.")
    finally:
        reboot.stop()
    check_image(checker, image, repo)


def partial_no_space_test(
    repo: Path, checker: Path, source: Path, qemu: str, temp: Path
) -> None:
    image = temp / "partial-no-space.img"
    host_source = temp / "space-fill-source"
    filler = host_source / "fill.bin"
    injector = checker.parent / "inject_transport"
    shutil.copy2(source, image)
    host_source.mkdir()

    free_before = count_free_data_blocks(image)
    if free_before < 3:
        raise RuntimeError("source image has too few blocks for no-space setup")
    with filler.open("wb") as stream:
        stream.truncate((free_before - 2) * 512)
    run_checked(
        [str(injector), str(image), str(host_source), "spacefill"], repo
    )
    if count_free_data_blocks(image) != 1:
        raise RuntimeError("no-space setup did not leave exactly one data block")
    check_image(checker, image, repo)

    vm = VirtualMachine(image, temp / "session-partial-no-space.log", qemu, -11)
    try:
        vm.start()
        start = vm.send_command(
            "run /transport/build/lib_test/test_no_space.bin"
        )
        vm.wait_for("NO-SPACE DIRTY TEST: PASS", start, 30.0)
    finally:
        vm.stop()

    expect_image_rejected(
        checker, image, repo, "filesystem has an unfinished mutation"
    )
    reboot = VirtualMachine(image, temp / "session-no-space-reboot.log", qemu, -12)
    try:
        reboot.start("MINI_OS: filesystem unavailable; system halted.")
    finally:
        reboot.stop()


def full_test(repo: Path, checker: Path, image: Path, qemu: str, temp: Path) -> None:
    first = VirtualMachine(image, temp / "session-1.log", qemu, 1)
    try:
        first.start()
        small_application_test(first)
        first.command_expect(
            "run /transport/build/lib_test/test_string.bin",
            "STRING/FORMAT TESTS PASSED",
        )
        first.command_expect(
            "run /transport/build/lib_test/test_heap.bin", "HEAP/STDLIB TESTS PASSED"
        )
        first.command_expect(
            "run /transport/build/lib_test/test_bss.bin", "BSS test: PASS"
        )
        first.command_expect(
            "run /transport/build/lib_test/test_stack.bin",
            "STACK CANARY TEST: PASS",
        )
        first.command_expect(
            "run /transport/build/lib_test/test_platform.bin",
            "PLATFORM TIMER TEST: PASS",
        )
        first.command_expect(
            "run /transport/build/lib_test/test_file.bin",
            "SYSCALL/STREAM TESTS PASSED",
        )
        filesystem_boundary_test(first)
        segment = first.command_expect("cat syslib.txt", "E2E-APPEND")
        if "E2E-BEGIN" not in segment or "E2E-END" not in segment:
            raise RuntimeError("cross-sector file markers were not all read back")
        first.command_expect("mkdir e2e", "Directory created: e2e")
        first.command_expect("mv syslib.txt e2e/moved.txt", "Renamed.")
        for index in range(14):
            first.command_expect(f"touch root{index:02d}", f"Created: root{index:02d}")
        first.command_expect("cd e2e", "/e2e > ")
        first.command_expect("ls", "moved.txt (f)")
    finally:
        first.stop()

    check_image(checker, image, repo)

    second = VirtualMachine(image, temp / "session-2.log", qemu, 2)
    try:
        second.start()
        segment = second.command_expect("cat /e2e/moved.txt", "E2E-APPEND")
        if "E2E-BEGIN" not in segment or "E2E-END" not in segment:
            raise RuntimeError("file contents did not survive a QEMU restart")
        second.command_expect("ls", "root13 (f)")
        for index in range(14):
            second.command_expect(f"rm /root{index:02d}", "Removed.")
        second.command_expect("mv /e2e/moved.txt /persist.txt", "Renamed.")
        second.command_expect("rm /persist.txt", "Removed.")
        second.command_expect("rm /e2e", "Removed.")
        second.command_expect("rm /README.TXT", "Removed.")
        second.command_expect("touch /replacement", "Created: /replacement")
        start = second.send_command("ls")
        segment = second.wait_for("/ > ", start, COMMAND_TIMEOUT)
        if "e2e (d)" in segment or "persist.txt (f)" in segment:
            raise RuntimeError("removed entries remain visible after cleanup")
    finally:
        second.stop()

    with image.open("rb") as stream:
        stream.seek(INODE_BITMAP_LBA * 512)
        first_bitmap_byte = stream.read(1)
    if len(first_bitmap_byte) != 1 or first_bitmap_byte[0] & 0x02 == 0:
        raise RuntimeError("freed README inode 1 was not reused")
    check_image(checker, image, repo)

    third = VirtualMachine(image, temp / "session-3.log", qemu, 3)
    try:
        third.start()
        third.command_expect("ls", "replacement (f)")
        start = third.send_command("ls")
        segment = third.wait_for("/ > ", start, COMMAND_TIMEOUT)
        if (
            "e2e (d)" in segment
            or "persist.txt (f)" in segment
            or "README.TXT (f)" in segment
            or "root13 (f)" in segment
        ):
            raise RuntimeError("removal did not survive a QEMU restart")
        third.command_expect("rm /replacement", "Removed.")
    finally:
        third.stop()

    check_image(checker, image, repo)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--checker", required=True, type=Path)
    parser.add_argument("--qemu", default="qemu-system-i386")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--program")
    parser.add_argument("--program-output", default="MINI_OS: app exited cleanly.")
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[1]
    source_image = args.image.resolve()
    checker = args.checker.resolve()
    if not source_image.is_file() or not checker.is_file():
        parser.error("--image and --checker must name existing files")

    with tempfile.TemporaryDirectory(prefix="mini-os-e2e-") as directory:
        temp = Path(directory)
        debug_image = temp / "mini_os_debug.img"
        if args.smoke:
            prepare_debug_image(repo, source_image, debug_image)
            vm = VirtualMachine(debug_image, temp / "smoke.log", args.qemu, 0)
            try:
                vm.start()
                smoke_test(vm, args.program, args.program_output)
            finally:
                vm.stop()
            check_image(checker, debug_image, repo)
        else:
            insufficient_memory_test(repo, source_image, args.qemu, temp)
            a20_failure_test(repo, source_image, args.qemu, temp)
            exception_handling_test(repo, source_image, args.qemu, temp)
            network_run_configuration_test(repo, source_image, args.qemu, temp)
            random_unavailable_test(repo, source_image, args.qemu, temp)
            forced_chs_test(repo, checker, source_image, args.qemu, temp)
            prepare_debug_image(repo, source_image, debug_image)
            memory_guard_failure_test(debug_image, args.qemu, temp)
            machine_compatibility_test(repo, checker, debug_image, args.qemu, temp)
            wrong_device_test(repo, checker, debug_image, args.qemu, temp)
            invalid_filesystem_boot_test(debug_image, args.qemu, temp)
            explicit_format_test(repo, checker, debug_image, args.qemu, temp)
            mutation_fault_test(repo, checker, debug_image, args.qemu, temp)
            read_fault_test(repo, checker, debug_image, args.qemu, temp)
            partial_no_space_test(repo, checker, debug_image, args.qemu, temp)
            full_test(repo, checker, debug_image, args.qemu, temp)

    print("QEMU filesystem regression: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"QEMU filesystem regression: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
