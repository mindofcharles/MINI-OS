#!/usr/bin/env python3
"""Dependency cache tests with synthetic archives and no network access."""

import hashlib
import io
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest


TOOL = Path(__file__).resolve().parents[1] / "tools/fetch_mbedtls.py"
PINNED_FILES = {"LICENSE": b"license", "library/md.c": b"source"}


class FetchMbedtls(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.cache = self.directory / "downloads/mbedtls/cache"
        self.archive = self.directory / "source.tar.bz2"
        self.checksums = self.directory / "files.sha256"
        self.write_checksums(PINNED_FILES)

    def write_checksums(self, files):
        self.checksums.write_text(
            "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n"
                    for name, data in files.items()),
            encoding="ascii",
        )

    def make_archive(self, *, symlink=False, source=b"source"):
        with tarfile.open(self.archive, "w:bz2") as archive:
            for name, content in (
                ("LICENSE", b"license"), ("library/md.c", source),
                ("unrelated.txt", b"not part of the dependency"),
            ):
                member = tarfile.TarInfo(f"upstream/{name}")
                if symlink and name == "library/md.c":
                    member.type = tarfile.SYMTYPE
                    member.linkname = "../../outside"
                    archive.addfile(member)
                else:
                    member.size = len(content)
                    archive.addfile(member, io.BytesIO(content))
        return hashlib.sha256(self.archive.read_bytes()).hexdigest()

    def fetch(self, checksum, *extra):
        return subprocess.run(
            [
                sys.executable, str(TOOL), "--file-checksums", str(self.checksums),
                self.archive.as_uri(), checksum,
                "upstream", str(self.cache), "LICENSE", "library/md.c", *extra,
            ],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_selects_only_required_files_and_reuses_cache(self):
        checksum = self.make_archive()
        result = self.fetch(checksum)
        self.assertEqual(result.returncode, 0, result.stderr)
        source = self.cache / "library/md.c"
        self.assertEqual(source.read_bytes(), b"source")
        self.assertFalse((self.cache / "unrelated.txt").exists())
        timestamp = source.stat().st_mtime_ns
        self.archive.unlink()
        result = self.fetch(checksum)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(source.stat().st_mtime_ns, timestamp)
        source.unlink()
        result = self.fetch(checksum)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("incomplete or unrecognized cache", result.stderr)

    def test_rejects_mismatched_checksum(self):
        self.make_archive()
        result = self.fetch("0" * 64)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SHA-256 mismatch", result.stderr)
        self.assertFalse(self.cache.exists())

    def test_does_not_publish_partial_cache(self):
        self.write_checksums({**PINNED_FILES, "library/missing.c": b"missing"})
        result = self.fetch(self.make_archive(), "library/missing.c")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("archive is missing", result.stderr)
        self.assertFalse(self.cache.exists())

    def test_rejects_archive_symlinks(self):
        result = self.fetch(self.make_archive(symlink=True))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not a regular file", result.stderr)
        self.assertFalse(self.cache.exists())

    def test_rejects_changed_extracted_file_with_matching_archive_checksum(self):
        result = self.fetch(self.make_archive(source=b"update"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SHA-256 mismatch for file library/md.c", result.stderr)
        self.assertFalse(self.cache.exists())

    def test_rejects_changed_cached_file_without_downloading(self):
        checksum = self.make_archive()
        result = self.fetch(checksum)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.archive.unlink()

        source = self.cache / "library/md.c"
        source.write_bytes(b"update")
        result = self.fetch(checksum)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SHA-256 mismatch for file library/md.c", result.stderr)
        self.assertNotIn("Downloading", result.stdout)
        self.assertEqual(source.read_bytes(), b"update")

    def test_requires_checksums_for_exactly_the_requested_files(self):
        checksum = self.make_archive()
        for files, message in (
            ({"LICENSE": b"license"}, "missing: library/md.c"),
            ({**PINNED_FILES, "unrelated.txt": b"unused"}, "extra: unrelated.txt"),
        ):
            with self.subTest(message=message):
                self.write_checksums(files)
                result = self.fetch(checksum)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertNotIn("Downloading", result.stdout)
                self.assertFalse(self.cache.exists())


if __name__ == "__main__":
    unittest.main()
