#!/usr/bin/env python3
"""Guest execution of the public TCP foundation, not TCP interoperability."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import sys
import tempfile

from qemu_e2e import VirtualMachine, check_image, prepare_debug_image


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--qemu", default="qemu-system-i386")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    source = args.image.resolve()
    if not source.is_file():
        parser.error("--image must name an existing image")
    with tempfile.TemporaryDirectory(prefix="mini-os-network-e1-") as directory:
        temp = Path(directory)
        for index, cpu in enumerate(("max,rdrand=on", "qemu32,rdrand=off")):
            image = temp / f"e1-{index}.img"
            log = temp / f"e1-{index}.log"
            vm = VirtualMachine(
                image, log, args.qemu, 220 + index,
                extra_args=[
                    "-accel", "tcg", "-cpu", cpu, "-netdev", "user,id=net0",
                    "-device", "ne2k_isa,netdev=net0,iobase=0x300,irq=9,"
                    "mac=52:54:00:12:34:56",
                ],
            )
            try:
                prepare_debug_image(repo, source, image)
                vm.start()
                for _ in range(2):
                    segment = vm.command_expect(
                        "run /transport/build/lib_test/test_tcp_contract.bin", "/ > "
                    )
                    if "E1 TCP CONTRACT TEST: PASS" not in segment:
                        raise RuntimeError(f"TCP foundation guest omitted PASS:\n{segment}")
                vm.command_expect("pwd", "/ > ")
            except Exception:
                if log.is_file():
                    artifacts = repo / "build/test-artifacts"
                    artifacts.mkdir(parents=True, exist_ok=True)
                    destination = Path(tempfile.mkdtemp(prefix="e1-failure-", dir=artifacts))
                    shutil.copy2(log, destination / log.name)
                    print(f"E1 failure log retained in {destination}", file=sys.stderr)
                raise
            finally:
                vm.stop()
            check_image(repo / "build/check_image", image, repo)
    print("E1 guest contract, repeated run, CPU configurations and guards: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"E1 guest contract: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
