#!/usr/bin/env python3
"""Fetch the pinned Mbed TLS files without involving the normal build."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import tarfile
import tempfile
from urllib.request import urlopen


MARKER = ".mini-os-archive-sha256"
SHA256 = re.compile(r"[0-9a-f]{64}")
BLOCK_SIZE = 128 * 1024


def read_file_checksums(manifest: Path, files: list[PurePosixPath]) -> dict[PurePosixPath, str]:
    checksums: dict[str, str] = {}
    for number, line in enumerate(manifest.read_text(encoding="ascii").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        checksum, separator, name = line.partition("  ")
        if not separator or not SHA256.fullmatch(checksum) or not name or name in checksums:
            raise ValueError(f"{manifest}:{number}: invalid or duplicate file checksum")
        checksums[name] = checksum

    expected = {file.as_posix() for file in files}
    missing = expected - checksums.keys()
    extra = checksums.keys() - expected
    if missing or extra:
        raise ValueError(
            f"{manifest}: file checksum list does not match the requested files "
            f"(missing: {', '.join(sorted(missing)) or 'none'}; "
            f"extra: {', '.join(sorted(extra)) or 'none'})"
        )
    return {name: checksums[name.as_posix()] for name in files}


def verify_files(directory: Path, checksums: dict[PurePosixPath, str]) -> None:
    for name, expected in checksums.items():
        path = directory / name
        if (path.is_symlink() or not path.is_file() or
                any((directory / parent).is_symlink() for parent in name.parents)):
            raise RuntimeError(f"file {name} is missing or not a regular file")
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(BLOCK_SIZE), b""):
                digest.update(block)
        if digest.hexdigest() != expected:
            raise RuntimeError(f"SHA-256 mismatch for file {name}")


def verify_cache(cache: Path, checksum: str, checksums: dict[PurePosixPath, str]) -> None:
    marker = cache / MARKER
    try:
        if (cache.is_symlink() or marker.is_symlink() or not marker.is_file() or
                marker.read_bytes() != f"{checksum}\n".encode("ascii")):
            raise RuntimeError("archive checksum marker missing or invalid")
        verify_files(cache, checksums)
    except (OSError, RuntimeError) as error:
        raise RuntimeError(
            f"incomplete or unrecognized cache at {cache}: {error}; move it aside and retry"
        ) from error


def fetch(url: str, checksum: str, root: str, cache: Path,
          names: list[str], manifest: Path) -> None:
    files = [PurePosixPath(name) for name in names]
    if not SHA256.fullmatch(checksum):
        raise ValueError("expected a lowercase SHA-256 digest")
    if any(
        name.is_absolute() or ".." in name.parts or not name.parts or name == PurePosixPath(MARKER)
        for name in files
    ) or len(set(files)) != len(files):
        raise ValueError("invalid archive file list")
    checksums = read_file_checksums(manifest, files)

    if cache.exists() or cache.is_symlink():
        verify_cache(cache, checksum, checksums)
        print(f"Mbed TLS cache is ready at {cache}")
        return

    cache.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".fetch-", dir=cache.parent) as directory:
        temporary = Path(directory)
        archive = temporary / "source.tar.bz2"
        digest = hashlib.sha256()
        print(f"Downloading Mbed TLS from {url}", flush=True)
        with urlopen(url, timeout=45) as response, archive.open("xb") as output:
            for block in iter(lambda: response.read(BLOCK_SIZE), b""):
                output.write(block)
                digest.update(block)
        if digest.hexdigest() != checksum:
            raise RuntimeError(f"SHA-256 mismatch for {url}")

        staging = temporary / "verified"
        staging.mkdir()
        with tarfile.open(archive, "r:bz2") as source:
            for name in files:
                try:
                    member = source.getmember(f"{root}/{name}")
                except KeyError:
                    raise RuntimeError(f"archive is missing {name}") from None
                if not member.isfile():
                    raise RuntimeError(f"archive member {name} is not a regular file")
                destination = staging / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                with source.extractfile(member) as entry, destination.open("xb") as output:
                    shutil.copyfileobj(entry, output)
        verify_files(staging, checksums)
        (staging / MARKER).write_text(checksum + "\n", encoding="ascii")
        try:
            staging.rename(cache)
        except OSError:
            # A parallel invocation may have published the same cache.
            if not cache.exists() and not cache.is_symlink():
                raise
            verify_cache(cache, checksum, checksums)
    print(f"Mbed TLS cache is ready at {cache}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--file-checksums", type=Path, required=True,
                        help="pinned SHA-256 checksums for the extracted files")
    parser.add_argument("url")
    parser.add_argument("sha256")
    parser.add_argument("archive_root")
    parser.add_argument("cache", type=Path)
    parser.add_argument("files", nargs="+")
    args = parser.parse_args()
    try:
        fetch(args.url, args.sha256, args.archive_root, args.cache,
              args.files, args.file_checksums)
    except (OSError, ValueError, RuntimeError, tarfile.TarError) as error:
        print(f"Mbed TLS setup failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
