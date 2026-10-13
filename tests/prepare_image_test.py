#!/usr/bin/env python3
"""Image preparation copies known archive members without unsafe extraction."""
import io
import json
import os
import subprocess
import shutil
import tarfile
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(os.environ.get("K_VM_PREPARE", "tools/prepare_linux_image.py")).resolve()


class PrepareImageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="k-vm-prepare-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.archive = self.root / "image.tar.gz"
        self.output = self.root / "output"

    def archive_with(self, link=False):
        with tarfile.open(self.archive, "w:gz") as tar:
            for name in ("bbl64.bin", "kernel-riscv64.bin", "root-riscv64.bin", "../../escape"):
                info = tarfile.TarInfo("diskimage-linux-riscv-2018-09-23/" + name)
                info.size = 512
                if link and name == "bbl64.bin":
                    info.type = tarfile.SYMTYPE
                    info.linkname = "/etc/passwd"
                    tar.addfile(info)
                else:
                    tar.addfile(info, io.BytesIO(bytes(512)))

    def run_tool(self):
        return subprocess.run(
            ["python3", SCRIPT, "--archive", self.archive, "--output", self.output, "--no-bootstrap"],
            capture_output=True, text=True, timeout=10,
        )

    def test_creates_offline_local_image_bundle(self):
        self.archive_with()
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        cfg = json.loads((self.output / "vm.cfg").read_text())
        self.assertEqual(cfg["machine"], "riscv64")
        self.assertEqual(cfg["drive0"]["file"], "root-riscv64.bin")
        self.assertEqual((self.output / "bbl64.bin").read_bytes(), bytes(512))
        self.assertFalse((self.root / "escape").exists())

    def test_rejects_symlink_assets(self):
        self.archive_with(link=True)
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

    def test_existing_bundle_not_overwritten(self):
        self.archive_with()
        self.output.mkdir()
        (self.output / "keep").write_text("keep")
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.output / "keep").read_text(), "keep")

    @unittest.skipUnless(shutil.which("debugfs") and shutil.which("mke2fs") and shutil.which("e2fsck"),
                         "ext filesystem tools are required")
    def test_bootstrap_reinstall_preserves_merged_usr_filesystem(self):
        tree = self.root / "rootfs"
        (tree / "usr/sbin").mkdir(parents=True)
        (tree / "sbin").symlink_to("usr/sbin")
        (tree / "usr/sbin/k-vm-init").write_text("old bootstrap\n")
        disk = self.root / "rootfs.raw"
        with disk.open("wb") as target:
            target.truncate(32 * 1024 * 1024)
        subprocess.run(["mke2fs", "-q", "-F", "-t", "ext4", "-d", tree, disk],
                       check=True, capture_output=True)
        installer = SCRIPT.parent / "install_guest_bootstrap.py"
        for _ in range(2):
            subprocess.run(["python3", installer, disk], check=True, capture_output=True)
        result = subprocess.run(["e2fsck", "-fn", disk], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
