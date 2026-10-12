#!/usr/bin/env python3
"""Synthetic staging tests: never load a Tencent library or private artifact."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

TOOL = Path(__file__).resolve().parents[1] / "tools/stage_tooltip.py"
spec = importlib.util.spec_from_file_location("stage_tooltip", TOOL)
assert spec and spec.loader
stager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stager)


class StagingTests(unittest.TestCase):
    fixture = b"unmodified synthetic ELF fixture"

    def stage(self, source, destination):
        with patch.object(stager, "ORIGINAL_SHA256", stager.digest(self.fixture)), \
                contextlib.redirect_stdout(io.StringIO()):
            stager.stage(source, destination)

    def test_exact_copy_exclusive_output_and_unchanged_input(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / "input", Path(directory) / "output"
            source.write_bytes(self.fixture)
            source.chmod(0o755)
            self.stage(source, output)
            self.assertEqual(source.read_bytes(), self.fixture)
            self.assertEqual(output.read_bytes(), self.fixture)
            self.assertEqual(output.stat().st_mode & 0o777, 0o755)
            with self.assertRaises(FileExistsError):
                self.stage(source, output)
            self.assertEqual(output.read_bytes(), self.fixture)

    def test_production_rejects_unknown_binary_without_output(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / "input", Path(directory) / "output"
            source.write_bytes(self.fixture)
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                stager.stage(source, output)
            self.assertFalse(output.exists())

    def test_rejects_in_place_and_links(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "input"
            source.write_bytes(self.fixture)
            with self.assertRaisesRegex(ValueError, "in-place"):
                self.stage(source, source)
            link = Path(directory) / "symlink"
            link.symlink_to(source)
            with self.assertRaisesRegex(ValueError, "in-place"):
                self.stage(source, link)
            link.unlink()
            os.link(source, link)
            with self.assertRaisesRegex(ValueError, "hard link"):
                self.stage(source, link)
            self.assertEqual(source.read_bytes(), self.fixture)

    def test_existing_symlink_does_not_overwrite_other_file(self):
        with tempfile.TemporaryDirectory() as directory:
            source, other, link = (Path(directory) / p for p in ("input", "other", "link"))
            source.write_bytes(self.fixture)
            other.write_bytes(b"keep")
            link.symlink_to(other)
            with self.assertRaises(FileExistsError):
                self.stage(source, link)
            self.assertEqual(other.read_bytes(), b"keep")


if __name__ == "__main__":
    unittest.main()
