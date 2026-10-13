#!/usr/bin/env python3
"""Check the Windows/WSL boundary without needing a Windows host."""
import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("wsl_launcher", Path(__file__).resolve().parents[1] / "tools/k_vm_wsl.py")
wsl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wsl)

class WslLauncherTest(unittest.TestCase):
    def translate(self, path):
        return "/translated/" + path.replace("\\", "/")

    def test_translates_only_host_paths_and_preserves_guest_arguments(self):
        result = wsl.translate_arguments([
            "sandbox", "--image", r"C:\images\Pi VM", "--config-dir=" + r"C:\Users\me\.pi\agent",
            "--map", r"data=D:\my files:rw", "--append", "root=/dev/vda rw", "--agent", "pi",
            "--", "--image", "prompt.png", "--config-dir", "guest-path",
        ], self.translate)
        self.assertEqual(result, [
            "sandbox", "--image", "/translated/C:/images/Pi VM",
            "--config-dir=/translated/C:/Users/me/.pi/agent", "--map", "data=/translated/D:/my files:rw",
            "--append", "root=/dev/vda rw", "--agent", "pi", "--", "--image", "prompt.png",
            "--config-dir", "guest-path",
        ])

    def test_updates_do_not_import_work_or_host_config(self):
        result = wsl.prepare_arguments(["update", "--version", "1.2.0"], {}, Path("/hosthome"), self.translate)
        self.assertEqual(result, ["update", "--version", "1.2.0", "--image", "/hosthome/.local/share/k-vm/images/pi-riscv64"])
        self.assertNotIn("--config-dir", result)

    def test_pi_defaults_use_host_configuration_and_image_environment(self):
        result = wsl.prepare_arguments(["sandbox", "--agent=pi", "--", "--model", "example"],
            {"K_VM_IMAGE": "local-image", "PI_CODING_AGENT_DIR": "agent-config"}, Path("/home"), self.translate)
        self.assertEqual(result, ["sandbox", "--agent=pi", "--image", "/translated/local-image",
            "--config-dir", "/translated/agent-config", "--", "--model", "example"])

    def test_missing_values_are_rejected(self):
        with self.assertRaises(ValueError):
            wsl.translate_arguments(["sandbox", "--image"], self.translate)

    def test_shell_does_not_import_configuration(self):
        result = wsl.prepare_arguments(["sandbox"], {}, Path("/home"), self.translate)
        self.assertNotIn("--config-dir", result)

    def test_native_long_option_abbreviations_are_normalized(self):
        self.assertEqual(wsl.translate_arguments(["sandbox", "--im", r"C:\images", "--ag=pi"], self.translate),
            ["sandbox", "--image", "/translated/C:/images", "--agent=pi"])
        with self.assertRaises(ValueError):
            wsl.translate_arguments(["sandbox", "--m", "x"], self.translate)

    def test_explicit_wsl_paths_do_not_become_windows_paths(self):
        self.assertEqual(wsl.translate_arguments(["update", "--image", "/home/user/pi-image"], self.translate),
            ["update", "--image", "/home/user/pi-image"])

    def test_distro_name_is_one_argument(self):
        self.assertEqual(wsl.wsl_prefix("Ubuntu 24.04"), ["wsl.exe", "--distribution", "Ubuntu 24.04"])

if __name__ == "__main__":
    unittest.main()
