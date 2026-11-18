#!/usr/bin/env python3
"""Unit tests for owned bootstrap-disk lifecycle; no QEMU or host mkfs needed."""
from contextlib import contextmanager
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import test_qemu


class BootstrapFixtureTests(unittest.TestCase):
    def setUp(self):
        self.workspace = tempfile.TemporaryDirectory()
        self.cwd = os.getcwd()
        os.chdir(self.workspace.name)
        Path("build").mkdir()
        self.developer = Path("build/disk.img")
        self.legacy = Path("build/test-bootstrap.img")
        self.developer.write_bytes(b"developer data -- do not alter")
        self.legacy.write_bytes(b"old exhausted fixture -- do not reuse or alter")
        self.created = []

    def tearDown(self):
        try:
            self.assertEqual(self.developer.read_bytes(), b"developer data -- do not alter")
            self.assertEqual(self.legacy.read_bytes(), b"old exhausted fixture -- do not reuse or alter")
        finally:
            os.chdir(self.cwd)
            self.workspace.cleanup()

    def mkext2(self, command):
        self.assertEqual(command[:2], ["python3", "tools/mkext2.py"])
        self.assertEqual(command[3:], ["32"])
        disk = Path(command[2])
        self.assertNotEqual(disk, self.developer)
        self.assertNotEqual(disk, self.legacy)
        self.assertEqual(disk.name, "disk.img")
        self.assertTrue(disk.parent.name.startswith("test-bootstrap-"))
        self.assertFalse(disk.exists())
        disk.write_bytes(b"fresh empty filesystem")
        self.created.append(disk)

    def test_repeated_runs_are_fresh_and_leave_old_images_untouched(self):
        with patch.object(test_qemu.subprocess, "check_call", self.mkext2):
            for _ in range(2):
                with test_qemu.ensure_disk() as disk:
                    self.assertEqual(Path(disk).read_bytes(), b"fresh empty filesystem")
                    Path(disk).write_bytes(b"first boot persistent data")
                    # Same context/disk remains intact for the rebuilt second boot.
                    self.assertEqual(Path(disk).read_bytes(), b"first boot persistent data")
                self.assertFalse(Path(disk).parent.exists())
        self.assertEqual(len(self.created), 2)
        self.assertNotEqual(*self.created)

    def test_exception_cleans_only_the_owned_fixture(self):
        with patch.object(test_qemu.subprocess, "check_call", self.mkext2):
            with self.assertRaisesRegex(RuntimeError, "guest failed"):
                with test_qemu.ensure_disk() as disk:
                    raise RuntimeError("guest failed")
        self.assertFalse(Path(disk).parent.exists())

    def test_formatter_failure_cleans_partial_fixture(self):
        def fail(command):
            self.mkext2(command)
            raise subprocess.CalledProcessError(1, command)
        with patch.object(test_qemu.subprocess, "check_call", fail):
            with self.assertRaises(subprocess.CalledProcessError):
                with test_qemu.ensure_disk():
                    self.fail("formatter failure must not yield a disk")
        self.assertFalse(self.created[0].parent.exists())

    def check_main_exit(self, code):
        def checks(disk):
            self.assertEqual(Path(disk).read_bytes(), b"fresh empty filesystem")
            Path(disk).write_bytes(b"stock boot data for reconstructed boot")
            self.assertEqual(Path(disk).read_bytes(), b"stock boot data for reconstructed boot")
            raise SystemExit(code)
        with patch.object(test_qemu.subprocess, "check_call", self.mkext2), \
             patch.object(test_qemu, "run_checks", checks):
            with self.assertRaises(SystemExit) as raised:
                test_qemu.main()
            self.assertEqual(raised.exception.code, code)
        self.assertFalse(self.created[0].parent.exists())

    def test_normal_success_exit_cleans_fixture(self):
        self.check_main_exit(0)

    def test_normal_failure_exit_cleans_fixture(self):
        self.check_main_exit(1)


if __name__ == "__main__":
    unittest.main()
