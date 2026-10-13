#!/usr/bin/env python3
"""Launch actual local guest instructions, with disk and file mappings."""
import json
import fcntl
import pty
import signal
import termios
import os
import struct
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

BINARY = Path(os.environ.get("K_VM_BINARY", "bazel-bin/k-vm")).resolve()


class SandboxTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="k-vm-sandbox-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.image = self.root / "image"
        self.image.mkdir()
        self.config = {
            "version": 1, "machine": "riscv64", "memory_size": 32,
            "bios": "bios.bin", "drive0": {"file": "rootfs.raw"},
        }
        (self.image / "bios.bin").write_bytes(struct.pack(
            "<5I", 0x400082B7, 0x00100313, 0x0062A023, 0x0002A223, 0x0000006F,
        ))
        (self.image / "rootfs.raw").write_bytes(bytes(4096))
        self.write_config()

    def write_config(self):
        (self.image / "vm.cfg").write_text(json.dumps(self.config))

    def run_vm(self, *options):
        return subprocess.run(
            [BINARY, "sandbox", "--image", self.image, *options],
            capture_output=True, text=True, timeout=10,
        )

    def test_local_image_starts_with_mapped_file_and_user_network(self):
        host = self.root / "host file.txt"
        host.write_text("host data")
        result = self.run_vm("--map", f"input={host}")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Power off.", result.stdout)
        self.assertEqual(host.read_text(), "host data")
        self.assertEqual((self.image / "rootfs.raw").read_bytes(), bytes(4096))

    def test_all_riscv_variants_start_from_local_images_without_network(self):
        for xlen in (32, 64, 128):
            with self.subTest(xlen=xlen):
                self.config["machine"] = f"riscv{xlen}"
                self.write_config()
                result = self.run_vm("--network", "none", "--memory", "64")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("Power off.", result.stdout)

    def test_mapping_directory_rw_and_existing_config(self):
        host = self.root / "workspace"
        host.mkdir()
        result = subprocess.run(
            [BINARY, "sandbox", "--image", self.image / "vm.cfg", "--map", f"extra={host}:rw"],
            capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_remote_boot_assets_rejected_before_download(self):
        self.config["bios"] = "http://127.0.0.1:1/bios.bin"
        self.write_config()
        result = self.run_vm()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("local", result.stderr)

    def test_missing_local_disk_is_rejected(self):
        (self.image / "rootfs.raw").unlink()
        result = self.run_vm()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("rootfs.raw", result.stderr)

    def test_unknown_disk_format_is_rejected(self):
        (self.image / "rootfs.raw").write_bytes(b"QFI\xfb" + bytes(4092))
        result = self.run_vm()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("raw", result.stderr)

    def test_boot_assets_must_not_overlap_initrd(self):
        bios = (self.image / "bios.bin").read_bytes()
        (self.image / "bios.bin").write_bytes(bios + bytes(10 * 1024 * 1024 - len(bios)))
        (self.image / "kernel.bin").write_bytes(bytes(7 * 1024 * 1024))
        (self.image / "initrd.bin").write_bytes(bytes(1024))
        self.config.update(kernel="kernel.bin", initrd="initrd.bin")
        self.write_config()
        result = self.run_vm()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overlap", result.stderr)

    def test_modern_firmware_identification_and_time_csrs(self):
        # Each read must execute in M-mode; illegal CSR access would trap and stall.
        for xlen in (32, 64, 128):
            self.config["machine"] = f"riscv{xlen}"
            self.write_config()
            reads = [0xf1102573, 0xf1202573, 0xf1302573, 0xc0102573]
            if xlen == 32:
                reads.append(0xc8102573)
            shutdown = [0x400082B7, 0x00100313, 0x0062A023, 0x0002A223, 0x0000006F]
            instructions = reads + shutdown
            (self.image / "bios.bin").write_bytes(struct.pack("<"+"I"*len(instructions), *instructions))
            result = self.run_vm("--network", "none")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Power off.", result.stdout)

    def test_supervisor_time_counter_is_enabled_by_firmware(self):
        self.config["machine"] = "riscv64"
        self.write_config()
        # Set mtvec, mcounteren.TIME, mepc and mstatus.MPP=S, then mret.
        # Trap handler only shuts down for supervisor ecall (cause 9), so an
        # illegal rdtime traps to an infinite loop and cannot falsely pass.
        instructions = [0x00000297, 0x03828293, 0x30529073, 0x00200293,
                        0x30629073, 0x00000297, 0x01c28293, 0x34129073,
                        0x000012b7, 0x0012d293, 0x3002a073, 0x30200073,
                        0xc0102373, 0x00000073, 0x342022f3, 0x00900313,
                        0x00629c63, 0x400082B7, 0x00100313, 0x0062A023,
                        0x0002A223, 0x0000006F, 0x0000006F]
        (self.image / "bios.bin").write_bytes(struct.pack("<"+"I"*len(instructions), *instructions))
        result = self.run_vm("--network", "none")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Power off.", result.stdout)

    def test_pc_images_are_not_supported(self):
        self.config["machine"] = "pc"
        self.write_config()
        result = self.run_vm()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pc", result.stderr)

    def test_acceleration_option_removed(self):
        result = self.run_vm("--no-accel")
        self.assertNotEqual(result.returncode, 0)
        result = subprocess.run([BINARY, "-h"], capture_output=True, text=True)
        self.assertNotIn("no-accel", result.stdout)

    def test_update_rejects_workspace_and_configuration_options(self):
        for options in (("--map", "extra="+str(self.root)), ("--config-dir", str(self.root)),
                        ("--agent", "pi"), ("--disk-mode", "rw"), ("--network", "none")):
            result = subprocess.run([BINARY, "update", "--image", self.image, *options],
                                    capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("update accepts", result.stderr)

    def test_invalid_launch_options_rejected(self):
        cases = [
            ("--network", "invalid"), ("--memory", "-1"),
            ("--memory", "12junk"), ("--map", "missing"),
            ("--map", "bad/tag=" + str(self.root)),
            ("--map", "same=" + str(self.root), "--map", "same=" + str(self.root)),
            ("--map", "missing=/nonexistent/k-vm"),
            ("--disk-mode", "invalid"), ("--invalid-option",), ("--agent", "codex"),
        ]
        for options in cases:
            with self.subTest(options=options):
                result = self.run_vm(*options)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr)

    def test_ctrlc_is_forwarded_and_sigterm_restores_host_terminal(self):
        (self.image / "bios.bin").write_bytes(struct.pack("<I", 0x0000006F))
        master, slave = pty.openpty()
        original = termios.tcgetattr(slave)
        original_flags = fcntl.fcntl(slave, fcntl.F_GETFL)
        proc = subprocess.Popen(
            [BINARY, "sandbox", "--image", self.image, "--network", "none"],
            stdin=slave, stdout=slave, stderr=slave,
        )
        try:
            deadline = time.monotonic() + 5
            while termios.tcgetattr(slave)[3] & termios.ICANON:
                self.assertIsNone(proc.poll(), "guest exited before initializing terminal")
                self.assertLess(time.monotonic(), deadline, "guest did not initialize terminal")
                time.sleep(0.01)
            self.assertFalse(termios.tcgetattr(slave)[3] & termios.ISIG,
                             "host terminal must forward Ctrl+C to the guest")
            os.write(master, b"\x03")
            time.sleep(0.05)
            self.assertIsNone(proc.poll(), "Ctrl+C must not stop the sandbox")
            proc.send_signal(signal.SIGTERM)
            self.assertEqual(proc.wait(timeout=5), 0)
            self.assertEqual(termios.tcgetattr(slave), original)
            self.assertEqual(fcntl.fcntl(slave, fcntl.F_GETFL), original_flags)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
            os.close(slave)

    def test_repeated_local_cold_start_reports_elapsed_time(self):
        # No timing threshold: this measures the small guest, not a full Linux boot.
        started = time.monotonic()
        for _ in range(3):
            result = self.run_vm("--network", "none")
            self.assertEqual(result.returncode, 0, result.stderr)
        print(f"Three local minimal guest cold starts: {time.monotonic() - started:.3f}s")


if __name__ == "__main__":
    unittest.main()
