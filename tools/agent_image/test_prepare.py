import importlib.util
import unittest
from pathlib import Path
spec = importlib.util.spec_from_file_location('prepare', Path(__file__).parents[1] / 'prepare_agent_image.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)

class ReleaseTests(unittest.TestCase):
    def test_base_image_supports_initialization_without_preinstalled_pi(self):
        caps = prepare.agent_capabilities(False)
        self.assertEqual(caps['agents'], ['shell'])
        self.assertEqual(caps['pi_bootstrap'], 1)

    def test_full_image_advertises_installed_pi(self):
        self.assertEqual(prepare.agent_capabilities(True)['agents'], ['shell', 'pi'])

    def test_selects_stable_instead_of_rc_or_longterm(self):
        releases = {'releases': [{'moniker':'mainline','version':'7.3-rc7'}, {'moniker':'stable','version':'7.2.10'}, {'moniker':'longterm','version':'6.18.52'}]}
        self.assertEqual(prepare.stable_kernel(releases), '7.2.10')

    def test_riscv_config_uses_firmware_and_builtin_root_drivers(self):
        config = prepare.vm_config()
        self.assertEqual(config['machine'], 'riscv64')
        self.assertEqual(config['bios'], 'opensbi.bin')
        self.assertIn('root=/dev/vda', config['cmdline'])
        self.assertNotIn('initrd', config)

if __name__ == '__main__': unittest.main()
