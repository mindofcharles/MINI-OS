#!/usr/bin/env python3
"""Build-policy, dependency, and fallback-linker regression checks."""

from __future__ import annotations

from pathlib import Path
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from typing import Optional


SECTOR_SIZE = 512
BOOT_ID_OFFSET = 504
BOOT_ID_SIZE = 6
SUPERBLOCK_LBA = 101
SUPER_DIRTY_OFFSET = 24
FAT_LBA = 103
INODE_START_LBA = 119
DATA_START_LBA = 375
INODE_SIZE = 64
INODE_NAME_CAP = 27
ELF2BIN_MAX_OBJECTS = 256
ELF2BIN_MAX_SYMBOLS = 4096
ELF2BIN_MAX_SECTIONS = 4096
ELF2BIN_MAX_RELOCATIONS = 32768
ELF2BIN_HARD_LIMITS = {
    "--max-output": 64 * 1024 * 1024,
    "--max-objects": 4096,
    "--max-symbols": 65536,
    "--max-sections": 65535,
    "--max-relocations": 1024 * 1024,
}


def run(
    command: list[str],
    cwd: Path,
    expect_success: bool = True,
    env: Optional[dict[str, str]] = None,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command, cwd=cwd, text=True, capture_output=True, env=env
    )
    if expect_success and result.returncode != 0:
        raise RuntimeError(
            f"command failed: {' '.join(command)}\n{result.stdout}{result.stderr}"
        )
    if not expect_success and result.returncode == 0:
        raise RuntimeError(f"command unexpectedly succeeded: {' '.join(command)}")
    return result


def copy_repository(source: Path, destination: Path) -> None:
    def ignore(directory: str, names: list[str]) -> set[str]:
        ignored = {".git", "build", "local", "__pycache__", ".DS_Store"}
        if Path(directory).name == "transport":
            ignored.add("build")
        return ignored.intersection(names)

    shutil.copytree(source, destination, ignore=ignore)


def platform_layout(repo: Path) -> dict[str, int]:
    values: dict[str, int] = {}
    definition = repo / "OS_src/kernel/platform_layout.def"
    pattern = re.compile(
        r"^PLATFORM_LAYOUT_CONST\(([A-Z0-9_]+),\s*(0x[0-9A-Fa-f]+|[0-9]+)\)$"
    )
    for line in definition.read_text(encoding="ascii").splitlines():
        match = pattern.fullmatch(line)
        if match is not None:
            values[match.group(1)] = int(match.group(2), 0)
    required = {
        "PLATFORM_CONFIGURED_MEMORY_BYTES",
        "PLATFORM_REQUIRED_CONVENTIONAL_END",
        "PLATFORM_REQUIRED_EXTENDED_END",
        "APP_IMAGE_BASE",
        "APP_IMAGE_SIZE",
        "APP_IMAGE_END",
        "APP_HEAP_SIZE",
        "APP_STACK_SIZE",
    }
    missing = required.difference(values)
    if missing:
        raise RuntimeError("platform layout is missing: " + ", ".join(sorted(missing)))
    return values


def reject_invalid_platform_layout(repo: Path) -> None:
    definition = repo / "OS_src/kernel/platform_layout.def"
    layout_tool = repo / "build/check_layout"
    original_stat = definition.stat()
    original = definition.read_text(encoding="ascii")
    cases = (
        (
            "PLATFORM_LAYOUT_CONST(NET_TX_BUFFER_BASE, 0x00028000)",
            "PLATFORM_LAYOUT_CONST(NET_TX_BUFFER_BASE, 0x00027000)",
            "overlaps",
        ),
        (
            "PLATFORM_LAYOUT_CONST(PLATFORM_REQUIRED_CONVENTIONAL_END, 0x00095000)",
            "PLATFORM_LAYOUT_CONST(PLATFORM_REQUIRED_CONVENTIONAL_END, 0x00094000)",
            "required conventional-memory end",
        ),
        (
            "PLATFORM_LAYOUT_CONST(PLATFORM_REQUIRED_EXTENDED_END, 0x001CB000)",
            "PLATFORM_LAYOUT_CONST(PLATFORM_REQUIRED_EXTENDED_END, 0x001CA000)",
            "required extended-memory end",
        ),
        (
            "PLATFORM_LAYOUT_CONST(PLATFORM_CONFIGURED_MEMORY_BYTES, 0x00400000)",
            "PLATFORM_LAYOUT_CONST(PLATFORM_CONFIGURED_MEMORY_BYTES, 0x00100000)",
            "firmware memory requirements are invalid",
        ),
    )

    try:
        for old_line, invalid_line, expected in cases:
            if original.count(old_line) != 1:
                raise RuntimeError(
                    "platform layout definition is unexpected: " + old_line
                )
            definition.write_text(
                original.replace(old_line, invalid_line), encoding="ascii"
            )
            layout_tool.unlink(missing_ok=True)
            rejected = run(["make", "check-layout"], repo, expect_success=False)
            if expected not in rejected.stdout + rejected.stderr:
                raise RuntimeError(
                    "invalid platform layout failed for an unexpected reason"
                )
        definition.write_text(original, encoding="ascii")
        layout_tool.unlink(missing_ok=True)
        run(["make", "check-layout"], repo)
    finally:
        definition.write_text(original, encoding="ascii")
        os.utime(
            definition,
            ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns),
        )


def timestamp(path: Path) -> int:
    return path.stat().st_mtime_ns


def make_source_newer(path: Path) -> None:
    time.sleep(1.05)
    os.utime(path, None)


def reject_invalid_raw_frame_layout(repo: Path) -> None:
    definition = repo / "transport/lib/net/raw.def"
    layout_tool = repo / "build/check_layout"
    original_stat = definition.stat()
    original = definition.read_text(encoding="ascii")
    frame_limit = "NET_RAW_CONST(NET_FRAME_MAX, 1514)"

    if original.count(frame_limit) != 1:
        raise RuntimeError("raw-frame maximum definition is unexpected")
    try:
        definition.write_text(
            original.replace(
                frame_limit,
                "NET_RAW_CONST(NET_FRAME_MAX, 4096)",
            ),
            encoding="ascii",
        )
        make_source_newer(definition)
        rejected = run(["make", "check-layout"], repo, expect_success=False)
        if "a frame buffer or canary constant is invalid" not in (
            rejected.stdout + rejected.stderr
        ):
            raise RuntimeError(
                "insufficient raw-frame alignment space failed unexpectedly"
            )
    finally:
        definition.write_text(original, encoding="ascii")
        layout_tool.unlink(missing_ok=True)
        run(["make", "check-layout"], repo)
        os.utime(
            definition,
            ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns),
        )


def assert_changed(before: dict[Path, int], paths: list[Path], label: str) -> None:
    unchanged = [str(path) for path in paths if timestamp(path) == before[path]]
    if unchanged:
        raise RuntimeError(f"{label} did not rebuild: {', '.join(unchanged)}")


def assert_unchanged(before: dict[Path, int], paths: list[Path], label: str) -> None:
    changed = [str(path) for path in paths if timestamp(path) != before[path]]
    if changed:
        raise RuntimeError(f"{label} rebuilt unexpectedly: {', '.join(changed)}")


def smoke(repo: Path) -> None:
    run(
        [
            sys.executable,
            "tests/qemu_e2e.py",
            "--image",
            "build/mini_os.img",
            "--checker",
            "build/check_image",
            "--smoke",
        ],
        repo,
    )


def expect_checker_failure(repo: Path, image: Path) -> None:
    run(["build/check_image", str(image)], repo, expect_success=False)


def verify_kernel_entry_stub(repo: Path) -> None:
    kernel = (repo / "build/kernel.bin").read_bytes()
    if b"MINI_OS_PHASE_C_FAULT_INJECTION_ONLY" in kernel:
        raise RuntimeError("Phase C test-only fault hooks entered the normal kernel")
    if len(kernel) >= 5 and kernel[0] == 0xE9:
        instruction_size = 5
        displacement = struct.unpack_from("<i", kernel, 1)[0]
    elif len(kernel) >= 2 and kernel[0] == 0xEB:
        instruction_size = 2
        displacement = struct.unpack_from("<b", kernel, 1)[0]
    else:
        raise RuntimeError("kernel image does not begin with a jump entry stub")
    target = instruction_size + displacement
    if target < instruction_size or target >= len(kernel):
        raise RuntimeError("kernel entry stub jumps outside the kernel image")


def verify_deterministic_network_backends(repo: Path) -> None:
    phase_b_marker = b"MINI_OS_PHASE_B_DETERMINISTIC_PLATFORM_ONLY"
    phase_d_marker = b"MINI_OS_PHASE_D_DETERMINISTIC_BACKEND_ONLY"
    run(["make", "test-network-host"], repo)
    phase_b_binary = repo / "build/network-phase-b/platform_test"
    phase_d_binary = repo / "build/network-phase-d/backend_test"
    production_image = (repo / "build/mini_os.img").read_bytes()
    if phase_b_marker not in phase_b_binary.read_bytes():
        raise RuntimeError("deterministic platform binary has no test-only marker")
    if phase_d_marker not in phase_d_binary.read_bytes():
        raise RuntimeError("deterministic packet backend has no test-only marker")
    if phase_b_marker in production_image:
        raise RuntimeError("deterministic platform marker entered production image")
    if phase_d_marker in production_image:
        raise RuntimeError("deterministic packet backend entered production image")

    symbols = run(["nm", "-u", str(phase_b_binary)], repo).stdout
    if "getchar" in symbols or "kbd_poll_key" in symbols:
        raise RuntimeError("deterministic wait test acquired a blocking/input syscall")

    probe = repo / "build/phase_b_header_probe.c"
    probe.write_text(
        "#include <platform.h>\n"
        "#include <net/net_platform.h>\n"
        "static int cancel(void *p) { return p != 0; }\n"
        "int main(void) { net_cancel_callback cb = cancel;\n"
        "return cb(0) + (int)clock_monotonic_ms(); }\n",
        encoding="ascii",
    )
    run(
        [
            "clang",
            "-target",
            "i386-unknown-none-elf",
            "-m32",
            "-march=i386",
            "-mno-sse",
            "-mno-mmx",
            "-ffreestanding",
            "-nostdlib",
            "-Itransport/lib",
            "-std=c90",
            "-pedantic-errors",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsyntax-only",
            str(probe),
        ],
        repo,
    )


def reject_corrupt_images(repo: Path) -> None:
    source = repo / "build/mini_os.img"
    corrupt = repo / "build/corrupt-boot-id.img"
    shutil.copyfile(source, corrupt)
    with corrupt.open("r+b") as image:
        image.seek(BOOT_ID_OFFSET)
        image.write(b"\0" * BOOT_ID_SIZE)
    result = run(["build/check_image", str(corrupt)], repo, expect_success=False)
    if "boot volume ID is missing" not in result.stderr:
        raise RuntimeError("checker did not reject a missing boot volume ID")

    corrupt = repo / "build/corrupt-dirty.img"
    shutil.copyfile(source, corrupt)
    with corrupt.open("r+b") as image:
        image.seek(SUPERBLOCK_LBA * SECTOR_SIZE + SUPER_DIRTY_OFFSET)
        image.write(struct.pack("<I", 1))
    result = run(["build/check_image", str(corrupt)], repo, expect_success=False)
    if "filesystem has an unfinished mutation" not in result.stderr:
        raise RuntimeError("checker did not diagnose the dirty mutation marker")

    corrupt = repo / "build/corrupt-fat.img"
    shutil.copyfile(source, corrupt)
    with corrupt.open("r+b") as image:
        image.seek(FAT_LBA * SECTOR_SIZE + 2 * 2)
        image.write(struct.pack("<H", 2))
    expect_checker_failure(repo, corrupt)

    for value, label in ((4096 * SECTOR_SIZE + 1, "capacity"),
                         (0xFFFFFFFF, "uint32")):
        corrupt = repo / f"build/corrupt-size-{label}.img"
        shutil.copyfile(source, corrupt)
        inode_offset = INODE_START_LBA * SECTOR_SIZE + INODE_SIZE
        with corrupt.open("r+b") as image:
            image.seek(inode_offset + 32)
            start_block = struct.unpack("<I", image.read(4))[0]
            image.seek(FAT_LBA * SECTOR_SIZE + start_block * 2)
            image.write(struct.pack("<H", 0))
            image.seek(inode_offset + 28)
            image.write(struct.pack("<III", value, 0, 0))
        expect_checker_failure(repo, corrupt)

    corrupt = repo / "build/corrupt-reserved-name.img"
    shutil.copyfile(source, corrupt)
    inode_offset = INODE_START_LBA * SECTOR_SIZE + INODE_SIZE
    with corrupt.open("r+b") as image:
        image.seek(INODE_START_LBA * SECTOR_SIZE + 32)
        root_block = struct.unpack("<I", image.read(4))[0]
        invalid_name = b".\0" + b"\0" * (INODE_NAME_CAP - 2)
        image.seek(inode_offset + 1)
        image.write(invalid_name)
        image.seek((DATA_START_LBA + root_block) * SECTOR_SIZE + 5)
        image.write(invalid_name)
    expect_checker_failure(repo, corrupt)


def reject_reserved_injector_names(repo: Path) -> None:
    source = repo / "build/mini_os.img"
    for index, name in enumerate((".", "..", "bad/name")):
        image = repo / f"build/inject-invalid-{index}.img"
        shutil.copyfile(source, image)
        run(
            ["build/inject_transport", str(image), "transport", name],
            repo,
            expect_success=False,
        )
        run(["build/check_image", str(image)], repo)


def reject_unsafe_injector_targets(repo: Path) -> None:
    source = repo / "build/mini_os.img"
    alias = repo / "build/inject-hardlink.img"
    os.link(source, alias)
    result = run(
        ["build/inject_transport", str(alias), "transport", "hardlinkprobe"],
        repo,
        expect_success=False,
    )
    if "multiple hard links" not in result.stderr:
        raise RuntimeError("injector did not diagnose a hard-linked target")
    run(["build/check_image", str(source)], repo)
    run(["build/check_image", str(alias)], repo)
    alias.unlink()


def injector_transaction_faults(repo: Path) -> None:
    source = repo / "build/mini_os.img"
    pristine = source.read_bytes()
    host_source = repo / "build/fault-source"
    host_source.mkdir()
    (host_source / "payload.bin").write_bytes(b"fault injection payload\n")
    fault_tool = repo / "build/inject_transport_fault"
    run(
        [
            "cc",
            "-O2",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DINJECT_FAULT_TEST",
            "-DCHECK_IMAGE_LIBRARY",
            "tools/inject_transport.c",
            "tools/check_image.c",
            "-o",
            str(fault_tool),
        ],
        repo,
    )

    probe = repo / "build/inject-fault-probe.img"
    shutil.copyfile(source, probe)
    result = run(
        [str(fault_tool), str(probe), str(host_source), "faultprobe"], repo
    )
    match = re.search(
        r"Fault-test sector writes: ([0-9]+)", result.stdout + result.stderr
    )
    if match is None or int(match.group(1)) == 0:
        raise RuntimeError("fault injector did not report its sector-write count")
    write_count = int(match.group(1))
    run(["build/check_image", str(probe)], repo)

    candidate = repo / "build/inject-fault-candidate.img"
    for variable in (
        "MINI_OS_INJECT_FAIL_BEFORE",
        "MINI_OS_INJECT_FAIL_AFTER",
    ):
        for sequence in range(1, write_count + 1):
            shutil.copyfile(source, candidate)
            environment = os.environ.copy()
            environment[variable] = str(sequence)
            run(
                [
                    str(fault_tool),
                    str(candidate),
                    str(host_source),
                    "faultprobe",
                ],
                repo,
                expect_success=False,
                env=environment,
            )
            if candidate.read_bytes() != pristine:
                raise RuntimeError(
                    f"injector changed the original after {variable}={sequence}"
                )
            run(["build/check_image", str(candidate)], repo)
            if list(candidate.parent.glob(candidate.name + ".inject-*")):
                raise RuntimeError("injector left a failed temporary image behind")

    for phase in ("chmod", "flush", "close", "check", "rename"):
        shutil.copyfile(source, candidate)
        environment = os.environ.copy()
        environment["MINI_OS_INJECT_FAIL_FINAL"] = phase
        run(
            [str(fault_tool), str(candidate), str(host_source), "faultprobe"],
            repo,
            expect_success=False,
            env=environment,
        )
        if candidate.read_bytes() != pristine:
            raise RuntimeError(
                f"injector changed the original after final-{phase} failure"
            )
        run(["build/check_image", str(candidate)], repo)
        if list(candidate.parent.glob(candidate.name + ".inject-*")):
            raise RuntimeError(
                f"injector left a final-{phase} temporary image behind"
            )

    dirty = repo / "build/inject-dirty.img"
    shutil.copyfile(source, dirty)
    with dirty.open("r+b") as image:
        image.seek(SUPERBLOCK_LBA * SECTOR_SIZE + SUPER_DIRTY_OFFSET)
        image.write(struct.pack("<I", 1))
    dirty_before = dirty.read_bytes()
    result = run(
        ["build/inject_transport", str(dirty), str(host_source), "faultprobe"],
        repo,
        expect_success=False,
    )
    if "unfinished mutation" not in result.stderr or dirty.read_bytes() != dirty_before:
        raise RuntimeError("injector did not safely reject a dirty filesystem")

    corrupt = repo / "build/inject-unrelated-corrupt.img"
    shutil.copyfile(source, corrupt)
    inode_offset = INODE_START_LBA * SECTOR_SIZE + INODE_SIZE
    with corrupt.open("r+b") as image:
        image.seek(inode_offset + 32)
        readme_block = struct.unpack("<I", image.read(4))[0]
        image.seek(FAT_LBA * SECTOR_SIZE + readme_block * 2)
        image.write(struct.pack("<H", readme_block))
    corrupt_before = corrupt.read_bytes()
    result = run(
        ["build/inject_transport", str(corrupt), str(host_source), "faultprobe"],
        repo,
        expect_success=False,
    )
    if (
        "failed the integrity check" not in result.stderr
        or corrupt.read_bytes() != corrupt_before
    ):
        raise RuntimeError(
            "injector committed an image that failed its pre-rename integrity gate"
        )
    if list(corrupt.parent.glob(corrupt.name + ".inject-*")):
        raise RuntimeError("injector left an integrity-rejected temporary image behind")


def compile_elf_probe(repo: Path, source: Path, output: Path) -> None:
    run(
        [
            "clang",
            "-target",
            "i386-unknown-none-elf",
            "-m32",
            "-march=i386",
            "-ffreestanding",
            "-nostdlib",
            "-O0",
            "-c",
            str(source),
            "-o",
            str(output),
        ],
        repo,
    )


def compile_nasm_probe(repo: Path, source: Path, output: Path) -> None:
    run(["nasm", "-f", "elf32", str(source), "-o", str(output)], repo)


def elf2bin_command(
    output: Path,
    base: int,
    objects: list[Path],
    max_output: int,
    max_objects: int = ELF2BIN_MAX_OBJECTS,
    max_symbols: int = ELF2BIN_MAX_SYMBOLS,
    max_sections: int = ELF2BIN_MAX_SECTIONS,
    max_relocations: int = ELF2BIN_MAX_RELOCATIONS,
) -> list[str]:
    return [
        "build/elf2bin",
        "--max-output",
        str(max_output),
        "--max-objects",
        str(max_objects),
        "--max-symbols",
        str(max_symbols),
        "--max-sections",
        str(max_sections),
        "--max-relocations",
        str(max_relocations),
        str(output),
        hex(base),
        *(str(path) for path in objects),
    ]


def elf32_sections(path: Path) -> list[tuple[int, int, int]]:
    data = path.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise RuntimeError(f"not an ELF32 object: {path}")
    section_offset = struct.unpack_from("<I", data, 32)[0]
    entry_size = struct.unpack_from("<H", data, 46)[0]
    entry_count = struct.unpack_from("<H", data, 48)[0]
    if entry_size != 40 or section_offset + entry_size * entry_count > len(data):
        raise RuntimeError(f"malformed ELF32 section table: {path}")
    sections = []
    for index in range(entry_count):
        offset = section_offset + index * entry_size
        section_type = struct.unpack_from("<I", data, offset + 4)[0]
        file_offset = struct.unpack_from("<I", data, offset + 16)[0]
        size = struct.unpack_from("<I", data, offset + 20)[0]
        sections.append((section_type, file_offset, size))
    return sections


def relocation_count(path: Path) -> int:
    return sum(size // 8 for kind, _, size in elf32_sections(path) if kind == 9)


def make_unsupported_relocation(source: Path, destination: Path) -> None:
    data = bytearray(source.read_bytes())
    for kind, offset, size in elf32_sections(source):
        if kind == 9 and size >= 8:
            information = struct.unpack_from("<I", data, offset + 4)[0]
            struct.pack_into("<I", data, offset + 4, (information & ~0xFF) | 3)
            destination.write_bytes(data)
            return
    raise RuntimeError("relocation probe did not contain a REL entry")


def reject_invalid_elf_inputs(repo: Path, base: int, image_size: int) -> None:
    output = repo / "build/rejected.bin"
    truncated = repo / "build/truncated.o"
    truncated.write_bytes(b"\x7fELF")
    run(
        elf2bin_command(output, base, [truncated], image_size),
        repo,
        expect_success=False,
    )

    invalid_offset = repo / "build/invalid-section-offset.o"
    shutil.copyfile(repo / "build/crt0.o", invalid_offset)
    with invalid_offset.open("r+b") as stream:
        stream.seek(32)
        stream.write(struct.pack("<I", 0xFFFFFFF0))
    run(
        elf2bin_command(output, base, [invalid_offset], image_size),
        repo,
        expect_success=False,
    )

    undefined_source = repo / "build/undefined.c"
    undefined_object = repo / "build/undefined.o"
    undefined_source.write_text(
        "extern void missing_symbol(void);\n"
        "void undefined_probe(void) { missing_symbol(); }\n",
        encoding="utf-8",
    )
    compile_elf_probe(repo, undefined_source, undefined_object)
    run(
        elf2bin_command(output, base, [undefined_object], image_size),
        repo,
        expect_success=False,
    )

    duplicate_objects = []
    for index in range(2):
        duplicate_source = repo / f"build/duplicate-{index}.c"
        duplicate_object = repo / f"build/duplicate-{index}.o"
        duplicate_source.write_text(
            f"int duplicate_symbol = {index + 1};\n", encoding="utf-8"
        )
        compile_elf_probe(repo, duplicate_source, duplicate_object)
        duplicate_objects.append(duplicate_object)
    run(
        elf2bin_command(output, base, duplicate_objects, image_size),
        repo,
        expect_success=False,
    )

    relocation_source = repo / "build/unsupported-relocation-source.c"
    relocation_object = repo / "build/unsupported-relocation-source.o"
    unsupported_object = repo / "build/unsupported-relocation.o"
    relocation_source.write_text(
        "int relocation_target;\n"
        "int *relocation_pointer = &relocation_target;\n",
        encoding="utf-8",
    )
    compile_elf_probe(repo, relocation_source, relocation_object)
    make_unsupported_relocation(relocation_object, unsupported_object)
    result = run(
        elf2bin_command(output, base, [unsupported_object], image_size),
        repo,
        expect_success=False,
    )
    if "unsupported relocation type" not in result.stderr:
        raise RuntimeError("elf2bin did not diagnose an unsupported relocation")

    overflow_source = repo / "build/output-overflow.c"
    overflow_object = repo / "build/output-overflow.o"
    overflow_source.write_text(
        f"unsigned char oversized_output[{image_size + 1}] = {{ 1 }};\n",
        encoding="utf-8",
    )
    compile_elf_probe(repo, overflow_source, overflow_object)
    run(
        elf2bin_command(output, base, [overflow_object], image_size),
        repo,
        expect_success=False,
    )

    weak_source = repo / "build/undefined-weak.c"
    weak_object = repo / "build/undefined-weak.o"
    weak_output = repo / "build/undefined-weak.bin"
    weak_source.write_text(
        "extern int optional_symbol __attribute__((weak));\n"
        "int *optional_pointer = &optional_symbol;\n",
        encoding="ascii",
    )
    compile_elf_probe(repo, weak_source, weak_object)
    run(elf2bin_command(weak_output, base, [weak_object], image_size), repo)
    if weak_output.read_bytes()[-4:] != b"\0\0\0\0":
        raise RuntimeError("elf2bin did not resolve an undefined weak symbol to zero")

    long_name = "symbol_" + "x" * 160
    long_name_source = repo / "build/long-symbol.c"
    long_name_object = repo / "build/long-symbol.o"
    long_name_source.write_text(f"int {long_name};\n", encoding="ascii")
    compile_elf_probe(repo, long_name_source, long_name_object)
    long_directory = repo / "build" / ("p" * 140)
    long_directory.mkdir(exist_ok=True)
    long_path_object = long_directory / "long-symbol.o"
    shutil.copyfile(long_name_object, long_path_object)
    run(elf2bin_command(output, base, [long_path_object], image_size), repo)

    constructor_source = repo / "build/constructor.c"
    constructor_object = repo / "build/constructor.o"
    constructor_source.write_text(
        "static void initialize(void) {}\n"
        "void (*constructor)(void) "
        "__attribute__((section(\".init_array\"))) = initialize;\n",
        encoding="ascii",
    )
    compile_elf_probe(repo, constructor_source, constructor_object)
    fallback_rejection = run(
        elf2bin_command(output, base, [constructor_object], image_size),
        repo,
        expect_success=False,
    )
    if "unsupported allocatable type" not in fallback_rejection.stderr:
        raise RuntimeError("elf2bin did not diagnose an unsupported constructor array")
    linker = shutil.which("ld.lld")
    if linker is None:
        raise RuntimeError("constructor-array rejection requires ld.lld")
    lld_rejection = run(
        lld_app_command(
            linker,
            repo,
            repo / "build/rejected-constructor.bin",
            [constructor_object],
            base,
            image_size,
        ),
        repo,
        expect_success=False,
    )
    if "application constructors are not supported" not in lld_rejection.stderr:
        raise RuntimeError("ld.lld did not diagnose an unsupported constructor array")

    tls_source = repo / "build/thread-local.c"
    tls_object = repo / "build/thread-local.o"
    tls_source.write_text("__thread int thread_value = 1;\n", encoding="ascii")
    compile_elf_probe(repo, tls_source, tls_object)
    fallback_rejection = run(
        elf2bin_command(output, base, [tls_object], image_size),
        repo,
        expect_success=False,
    )
    if "unsupported allocatable flags" not in fallback_rejection.stderr:
        raise RuntimeError("elf2bin did not diagnose unsupported thread-local storage")
    lld_rejection = run(
        lld_app_command(
            linker,
            repo,
            repo / "build/rejected-thread-local.bin",
            [tls_object],
            base,
            image_size,
        ),
        repo,
        expect_success=False,
    )
    if "application thread-local storage is not supported" not in lld_rejection.stderr:
        raise RuntimeError("ld.lld did not diagnose unsupported thread-local storage")


def check_elf2bin_capacities(repo: Path, base: int) -> None:
    output = repo / "build/capacity.bin"
    one_source = repo / "build/capacity-one.asm"
    one_object = repo / "build/capacity-one.o"
    two_source = repo / "build/capacity-two.asm"
    two_object = repo / "build/capacity-two.o"
    relocation_source = repo / "build/capacity-relocations.asm"
    relocation_object = repo / "build/capacity-relocations.o"

    one_source.write_text(
        "bits 32\nsection .text\nglobal symbol_one\nsymbol_one: ret\n",
        encoding="ascii",
    )
    two_source.write_text(
        "bits 32\nsection .text\nglobal symbol_one\nglobal symbol_two\n"
        "symbol_one: ret\nsymbol_two: ret\n",
        encoding="ascii",
    )
    relocation_source.write_text(
        "bits 32\nsection .data\nglobal relocation_probe\n"
        "relocation_probe: dd relocation_target, relocation_target\n"
        "relocation_target: dd 0\n",
        encoding="ascii",
    )
    compile_nasm_probe(repo, one_source, one_object)
    compile_nasm_probe(repo, two_source, two_object)
    compile_nasm_probe(repo, relocation_source, relocation_object)

    section_count = len(elf32_sections(one_object))
    run(
        elf2bin_command(
            output, base, [one_object], 16, max_objects=1,
            max_symbols=1, max_sections=section_count, max_relocations=1,
        ),
        repo,
    )
    run(
        elf2bin_command(output, base, [one_object, two_object], 16, max_objects=1),
        repo,
        expect_success=False,
    )
    run(
        elf2bin_command(output, base, [two_object], 16, max_symbols=1),
        repo,
        expect_success=False,
    )
    if section_count <= 1:
        raise RuntimeError("section-capacity probe has too few sections")
    run(
        elf2bin_command(
            output, base, [one_object], 16, max_sections=section_count - 1
        ),
        repo,
        expect_success=False,
    )

    relocations = relocation_count(relocation_object)
    if relocations < 2:
        raise RuntimeError("relocation-capacity probe has too few relocations")
    run(
        elf2bin_command(
            output, base, [relocation_object], 16,
            max_relocations=relocations,
        ),
        repo,
    )
    run(
        elf2bin_command(
            output, base, [relocation_object], 16,
            max_relocations=relocations - 1,
        ),
        repo,
        expect_success=False,
    )
    for option, hard_limit in ELF2BIN_HARD_LIMITS.items():
        for invalid_value in (0, hard_limit + 1):
            run(
                [
                    "build/elf2bin",
                    option,
                    str(invalid_value),
                    str(output),
                    hex(base),
                    str(one_object),
                ],
                repo,
                expect_success=False,
            )


def lld_app_command(
    linker: str, repo: Path, output: Path, objects: list[Path],
    base: int, image_size: int
) -> list[str]:
    return [
        linker,
        "-m",
        "elf_i386",
        "--orphan-handling=error",
        f"--defsym=APP_LINK_BASE={hex(base)}",
        f"--defsym=APP_LINK_SIZE={hex(image_size)}",
        "-T",
        str(repo / "transport/app.ld"),
        "--oformat",
        "binary",
        *(str(path) for path in objects),
        "-o",
        str(output),
    ]


def smoke_exact_binary(repo: Path, binary: Path, label: str) -> None:
    image = repo / f"build/exact-{label}.img"
    source = repo / f"build/exact-{label}-source"
    shutil.copyfile(repo / "build/mini_os.img", image)
    source.mkdir()
    shutil.copyfile(binary, source / "exact.bin")
    run(
        ["build/inject_transport", str(image), str(source), "phasea"],
        repo,
    )
    run(["build/check_image", str(image)], repo)
    run(
        [
            sys.executable,
            "tests/qemu_e2e.py",
            "--image",
            str(image),
            "--checker",
            "build/check_image",
            "--smoke",
            "--program",
            "/phasea/exact.bin",
        ],
        repo,
    )


def verify_partial_sector_bss_zeroing(
    repo: Path, base: int, image_size: int
) -> None:
    linker = shutil.which("ld.lld")
    if linker is None:
        raise RuntimeError("Phase A BSS-tail test requires ld.lld")

    assembly = repo / "build/bss-tail.asm"
    object_path = repo / "build/bss-tail.o"
    binary = repo / "build/bss-tail.bin"
    source = repo / "build/bss-tail-source"
    image = repo / "build/bss-tail.img"
    assembly.write_text(
        "bits 32\nsection .text\nglobal _start\n"
        "_start: cmp byte [bss_probe], 0\n"
        "jne failed\nret\nfailed: jmp failed\n"
        "section .bss\nalign 16\nbss_probe: resb 1\n",
        encoding="ascii",
    )
    compile_nasm_probe(repo, assembly, object_path)
    run(
        lld_app_command(
            linker, repo, binary, [object_path], base, image_size
        ),
        repo,
    )
    logical_size = binary.stat().st_size
    if logical_size == 0 or logical_size >= SECTOR_SIZE:
        raise RuntimeError("BSS-tail probe must occupy one partial sector")

    shutil.copyfile(repo / "build/mini_os.img", image)
    source.mkdir()
    shutil.copyfile(binary, source / "tail.bin")
    injected = run(
        ["build/inject_transport", str(image), str(source), "phaseabss"], repo
    )
    match = re.search(
        r"Injected file 'tail\.bin' \((\d+) bytes, (\d+) sectors\) "
        r"-> inode \d+, LBA (\d+)",
        injected.stdout + injected.stderr,
    )
    if match is None:
        raise RuntimeError("cannot locate the injected BSS-tail probe")
    if int(match.group(1)) != logical_size or int(match.group(2)) != 1:
        raise RuntimeError("injected BSS-tail probe has unexpected metadata")

    data_lba = int(match.group(3))
    with image.open("r+b") as stream:
        stream.seek(data_lba * SECTOR_SIZE + logical_size)
        stream.write(b"\xCC" * (SECTOR_SIZE - logical_size))
    run(["build/check_image", str(image)], repo)
    run(
        [
            sys.executable,
            "tests/qemu_e2e.py",
            "--image",
            str(image),
            "--checker",
            "build/check_image",
            "--smoke",
            "--program",
            "/phaseabss/tail.bin",
        ],
        repo,
    )


def phase_a_image_boundaries(repo: Path, base: int, image_size: int) -> None:
    exact_source = repo / "build/exact-limit.asm"
    exact_object = repo / "build/exact-limit.o"
    over_source = repo / "build/over-limit.asm"
    over_object = repo / "build/over-limit.o"
    bss_source = repo / "build/bss-overflow.asm"
    bss_object = repo / "build/bss-overflow.o"
    unresolved_source = repo / "build/lld-unresolved.asm"
    unresolved_object = repo / "build/lld-unresolved.o"
    exact_elf2bin = repo / "build/exact-elf2bin.bin"

    exact_source.write_text(
        "bits 32\nsection .text\nglobal _start\n_start: ret\n"
        f"times {image_size} - ($ - $$) db 0x90\n",
        encoding="ascii",
    )
    over_source.write_text(
        "bits 32\nsection .text\nglobal _start\n_start: ret\n"
        f"times {image_size + 1} - ($ - $$) db 0x90\n",
        encoding="ascii",
    )
    bss_source.write_text(
        "bits 32\nsection .text\nglobal _start\n_start: ret\n"
        f"section .bss\nresb {image_size}\n",
        encoding="ascii",
    )
    unresolved_source.write_text(
        "bits 32\nsection .text\nglobal _start\nextern missing_symbol\n"
        "_start: call missing_symbol\nret\n",
        encoding="ascii",
    )
    compile_nasm_probe(repo, exact_source, exact_object)
    compile_nasm_probe(repo, over_source, over_object)
    compile_nasm_probe(repo, bss_source, bss_object)
    compile_nasm_probe(repo, unresolved_source, unresolved_object)

    run(
        elf2bin_command(exact_elf2bin, base, [exact_object], image_size),
        repo,
    )
    if exact_elf2bin.stat().st_size != image_size:
        raise RuntimeError("elf2bin exact-limit output has the wrong size")
    run(
        elf2bin_command(
            repo / "build/rejected-over.bin", base, [over_object], image_size
        ),
        repo,
        expect_success=False,
    )
    run(
        elf2bin_command(
            repo / "build/rejected-bss.bin", base, [bss_object], image_size
        ),
        repo,
        expect_success=False,
    )

    linker = shutil.which("ld.lld")
    if linker is None:
        raise RuntimeError("Phase A boundary tests require ld.lld")
    exact_lld = repo / "build/exact-lld.bin"
    run(lld_app_command(linker, repo, exact_lld, [exact_object], base, image_size), repo)
    if exact_lld.stat().st_size != image_size:
        raise RuntimeError("ld.lld exact-limit output has the wrong size")
    run(
        lld_app_command(
            linker, repo, repo / "build/rejected-over-lld.bin",
            [over_object], base, image_size,
        ),
        repo,
        expect_success=False,
    )
    run(
        lld_app_command(
            linker, repo, repo / "build/rejected-bss-lld.bin",
            [bss_object], base, image_size,
        ),
        repo,
        expect_success=False,
    )
    run(
        lld_app_command(
            linker, repo, repo / "build/rejected-unresolved-lld.bin",
            [unresolved_object], base, image_size,
        ),
        repo,
        expect_success=False,
    )
    smoke_exact_binary(repo, exact_elf2bin, "elf2bin")
    smoke_exact_binary(repo, exact_lld, "lld")


def verify_per_application_libraries(repo: Path) -> None:
    net_directory = repo / "transport/lib/net"
    ssh_directory = repo / "transport/lib/ssh"
    ping_source = repo / "transport/apps/ping.c"
    ssh_app_source = repo / "transport/apps/ssh.c"
    net_source = net_directory / "dependency_probe.c"
    ssh_source = ssh_directory / "dependency_probe.c"

    net_directory.mkdir(exist_ok=True)
    ssh_directory.mkdir(exist_ok=True)

    hello_binary = repo / "transport/build/apps/hello.bin"
    hello_object = repo / "build/transport/apps/hello.o"
    netdiag_binary = repo / "transport/build/apps/netdiag.bin"
    netdiag_object = repo / "build/transport/apps/netdiag.o"
    ping_binary = repo / "transport/build/apps/ping.bin"
    ping_object = repo / "build/transport/apps/ping.o"
    compiler_runtime = repo / "build/compiler_rt.o"
    hello_binary.unlink()
    hello_object.unlink()
    compiler_runtime.unlink(missing_ok=True)
    for network_object in (repo / "build/transport/lib/net").glob("*.o"):
        network_object.unlink()
    hello = run(["make", "transport/build/apps/hello.bin"], repo)
    hello_output = hello.stdout + hello.stderr
    if "compiler_rt.c" in hello_output or "transport/lib/net/" in hello_output:
        raise RuntimeError("ordinary application acquired network-only objects")
    if compiler_runtime.exists() or any(
        (repo / "build/transport/lib/net").glob("*.o")
    ):
        raise RuntimeError("ordinary application built network-only support")

    netdiag_binary.unlink(missing_ok=True)
    netdiag_object.unlink(missing_ok=True)
    netdiag = run(["make", "app", "APP=netdiag.c"], repo)
    netdiag_output = netdiag.stdout + netdiag.stderr
    if (
        "dependency_probe.c" in netdiag_output
        or "transport/lib/net/net.c" in netdiag_output
        or (repo / "build/transport/lib/net/dependency_probe.o").exists()
        or (repo / "build/transport/lib/net/net.o").exists()
    ):
        raise RuntimeError("raw network application acquired IPv4 objects")

    net_source.write_text(
        "unsigned int net_dependency_probe(void)\n{\n"
        "    volatile unsigned long long value = 0x100000002ULL;\n"
        "    volatile unsigned long long divisor = 3ULL;\n"
        "    return (unsigned int)(value / divisor);\n}\n",
        encoding="ascii",
    )
    ungrouped = run(["make", "-n", "app", "APP=netdiag.c"], repo,
                    expect_success=False)
    if "network library sources have no object group" not in (
        ungrouped.stdout + ungrouped.stderr
    ):
        raise RuntimeError("build did not reject an ungrouped network source")

    duplicate_override = (
        "NET_IPV4_LIB_SRCS=transport/lib/net/net.c "
        "transport/lib/net/raw.c transport/lib/net/dependency_probe.c"
    )
    duplicate = run(
        ["make", "-n", duplicate_override, "app", "APP=netdiag.c"],
        repo,
        expect_success=False,
    )
    if "network library sources assigned to multiple groups" not in (
        duplicate.stdout + duplicate.stderr
    ):
        raise RuntimeError("build did not reject duplicate network grouping")

    missing_override = (
        "NET_IPV4_LIB_SRCS=transport/lib/net/net.c "
        "transport/lib/net/dependency_probe.c transport/lib/net/missing_probe.c"
    )
    missing = run(
        ["make", "-n", missing_override, "app", "APP=netdiag.c"],
        repo,
        expect_success=False,
    )
    if "grouped network library sources do not exist" not in (
        missing.stdout + missing.stderr
    ):
        raise RuntimeError("build did not reject a nonexistent grouped source")

    ssh_source.write_text(
        "int ssh_dependency_probe(void)\n{\n    return 7;\n}\n",
        encoding="ascii",
    )
    ping_source.write_text(
        "int net_dependency_probe(void);\n"
        "int main(void)\n{\n    return net_dependency_probe() == 0U;\n}\n",
        encoding="ascii",
    )
    ssh_app_source.write_text(
        "int net_dependency_probe(void);\nint ssh_dependency_probe(void);\n"
        "int main(void)\n{\n"
        "    return net_dependency_probe() == 0U || ssh_dependency_probe() != 7;\n"
        "}\n",
        encoding="ascii",
    )

    protocol_override = (
        "NET_IPV4_LIB_SRCS=transport/lib/net/net.c "
        "transport/lib/net/dependency_probe.c"
    )
    ping = run(["make", protocol_override, "app", "APP=ping.c"], repo)
    ping_output = ping.stdout + ping.stderr
    ping_lines = ping_output.splitlines()
    library_compile = next(
        (
            line
            for line in ping_lines
            if " -c " in line and "dependency_probe.c" in line
        ),
        "",
    )
    application_compile = next(
        (
            line
            for line in ping_lines
            if " -c " in line and "transport/apps/ping.c" in line
        ),
        "",
    )
    if (
        "-std=gnu11" not in library_compile
        or "-std=c90" not in application_compile
    ):
        raise RuntimeError("network library and application language policies diverged")
    if not all(
        flag in library_compile for flag in ("-Wall", "-Wextra", "-Werror")
    ):
        raise RuntimeError("network library warnings are not fatal")
    if "-pedantic-errors" not in application_compile:
        raise RuntimeError("network application is not strict C90")
    if not (repo / "build/compiler_rt.o").is_file() or not (
        repo / "build/transport/lib/net/dependency_probe.o"
    ).is_file():
        raise RuntimeError(
            "network application did not acquire its private objects\n" + ping_output
        )
    if not (repo / "build/transport/lib/net/net.o").is_file():
        raise RuntimeError("IPv4 application did not acquire its context object")
    context_symbols = run(
        ["nm", "-S", "build/transport/lib/net/net.o"], repo
    ).stdout
    context_match = re.search(
        r"^[0-9A-Fa-f]+\s+([0-9A-Fa-f]+)\s+[Bb]\s+net_global_context$",
        context_symbols,
        re.MULTILINE,
    )
    if context_match is None:
        raise RuntimeError("network context is not stored in BSS")
    context_size = int(context_match.group(1), 16)
    if context_size < 2 * 1514 or context_size > 4096:
        raise RuntimeError("network context has an unexpected memory footprint")
    if (repo / "build/transport/lib/ssh/dependency_probe.o").exists():
        raise RuntimeError("non-SSH network application acquired SSH objects")

    ping_binary.unlink()
    ping_object.unlink()
    fallback_ping = run(
        [
            "make",
            "LLD=__missing_ld_lld__",
            protocol_override,
            "app",
            "APP=ping.c",
        ],
        repo,
    )
    if "with elf2bin" not in fallback_ping.stdout + fallback_ping.stderr:
        raise RuntimeError("IPv4 application fallback did not use elf2bin")

    platform_object = repo / "build/transport/lib/net/platform.o"
    time_object = repo / "build/transport/lib/net/time.o"
    if not platform_object.is_file() or not time_object.is_file():
        raise RuntimeError("network platform objects were not compiled")
    platform_symbols = run(["nm", "-u", str(platform_object)], repo).stdout
    if "kbd_poll_key" not in platform_symbols or "getchar" in platform_symbols:
        raise RuntimeError("production cancellation adapter is not nonblocking")

    run(["make", protocol_override, "app", "APP=ssh.c"], repo)
    if not (repo / "build/transport/lib/ssh/dependency_probe.o").is_file():
        raise RuntimeError("SSH application did not acquire its private objects")

    ping_source.unlink()
    ssh_app_source.unlink()
    net_source.unlink()
    ssh_source.unlink()


def main() -> int:
    source = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="mini-os-build-test-") as directory:
        repo = Path(directory) / "repo"
        copy_repository(source, repo)
        layout = platform_layout(repo)
        app_base = layout["APP_IMAGE_BASE"]
        app_size = layout["APP_IMAGE_SIZE"]
        if layout["APP_IMAGE_END"] != app_base + app_size:
            raise RuntimeError("application image layout is inconsistent")

        run(["make", "network-phase0-check"], repo)
        reject_invalid_platform_layout(repo)
        reject_invalid_raw_frame_layout(repo)
        run(["make", "clean"], repo)
        clean_build = run(["make"], repo)
        output = clean_build.stdout + clean_build.stderr
        if "-std=gnu11" not in output:
            raise RuntimeError("runtime library was not compiled as modern C")
        if "-std=c90" not in output or "-pedantic-errors" not in output:
            raise RuntimeError("applications were not compiled with strict C90 diagnostics")
        run(["build/check_layout"], repo)
        verify_kernel_entry_stub(repo)
        run(["build/check_image", "build/mini_os.img"], repo)
        verify_deterministic_network_backends(repo)
        run(["make", "test-network-abi"], repo)
        reject_corrupt_images(repo)
        reject_reserved_injector_names(repo)
        reject_unsafe_injector_targets(repo)
        injector_transaction_faults(repo)
        reject_invalid_elf_inputs(repo, app_base, app_size)
        check_elf2bin_capacities(repo, app_base)
        smoke(repo)
        phase_a_image_boundaries(repo, app_base, app_size)
        verify_partial_sector_bss_zeroing(repo, app_base, app_size)

        probe = repo / "transport/apps/c99_probe.c"
        probe.write_text(
            "int main(void) { for (int i = 0; i < 1; i++) {} return 0; }\n",
            encoding="utf-8",
        )
        failed_probe = run(
            ["make", "app", "APP=c99_probe.c"], repo, expect_success=False
        )
        probe.unlink()
        if "for loop initial declarations" not in (
            failed_probe.stdout + failed_probe.stderr
        ) and "C99" not in (failed_probe.stdout + failed_probe.stderr):
            raise RuntimeError("strict-C90 negative probe failed for an unexpected reason")

        tracked = [
            repo / "build/kernel.bin",
            repo / "build/minilibc.o",
            repo / "transport/build/apps/hello.bin",
            repo / "build/mini_os.img",
        ]
        before = {path: timestamp(path) for path in tracked}
        run(["make"], repo)
        assert_unchanged(before, tracked, "no-op incremental build")

        header = repo / "transport/lib/string.h"
        with header.open("a", encoding="utf-8") as stream:
            stream.write("\n/* dependency regression marker */\n")
        make_source_newer(header)
        before = {path: timestamp(path) for path in tracked}
        run(["make"], repo)
        assert_changed(before, tracked[1:], "runtime-header dependency")
        assert_unchanged(before, tracked[:1], "runtime-header dependency")

        driver = repo / "OS_src/kernel/drivers.asm"
        with driver.open("a", encoding="utf-8") as stream:
            stream.write("\n; dependency regression marker\n")
        make_source_newer(driver)
        kernel_paths = [repo / "build/kernel.bin", repo / "build/boot.bin", repo / "build/mini_os.img"]
        app_path = repo / "transport/build/apps/hello.bin"
        before_kernel = {path: timestamp(path) for path in kernel_paths}
        before_app = timestamp(app_path)
        run(["make"], repo)
        assert_changed(before_kernel, kernel_paths, "kernel-include dependency")
        if timestamp(app_path) != before_app:
            raise RuntimeError("kernel-only change rebuilt an unrelated application")

        verify_per_application_libraries(repo)

        run(["make", "clean"], repo)
        fallback = run(["make", "LLD=__missing_ld_lld__"], repo)
        fallback_output = fallback.stdout + fallback.stderr
        if "linking 'transport/build" not in fallback_output or "with elf2bin" not in fallback_output:
            raise RuntimeError("fallback build did not use elf2bin")
        run(["build/check_image", "build/mini_os.img"], repo)
        smoke(repo)

    print("Build regression: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"Build regression: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
