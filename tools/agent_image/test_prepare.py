import importlib.util
import sys
import unittest
import tempfile
from unittest.mock import Mock, patch
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parents[1]))
spec = importlib.util.spec_from_file_location('prepare', Path(__file__).parents[1] / 'prepare_agent_image.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)

class ReleaseTests(unittest.TestCase):
    def test_linux_version_change_keeps_opensbi_cache(self):
        def kernel(cache, release, directory):
            (directory / 'kernel.bin').write_bytes(b'kernel')
            (directory / 'kernel.config').write_text('config')
        def firmware(cache, release, directory):
            (directory / 'opensbi.bin').write_bytes(b'firmware')
        with tempfile.TemporaryDirectory() as tmp, \
             patch.object(prepare.boot, 'compiler_identity', return_value='gcc fixture'), \
             patch.object(prepare.boot, '_build_kernel', side_effect=kernel) as linux, \
             patch.object(prepare.boot, '_build_opensbi', side_effect=firmware) as opensbi:
            versions = prepare.boot.load_versions()
            prepare.boot.build_boot(Path(tmp), versions)
            versions['linux']['version'] = '7.2.11'
            prepare.boot.build_boot(Path(tmp), versions)
            self.assertEqual(linux.call_count, 2)
            self.assertEqual(opensbi.call_count, 1)

    def test_boot_cache_hit_does_not_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            def build(directory):
                (directory / 'kernel.bin').write_bytes(b'kernel')
            builder = Mock(side_effect=build)
            args = (Path(tmp), 'linux', {'version': '7.2.10'}, ['kernel.bin'], builder)
            first = prepare.boot.cached_component(*args)
            self.assertEqual(first, prepare.boot.cached_component(*args))
            self.assertEqual(builder.call_count, 1)

    def test_version_change_and_corrupt_artifact_rebuild(self):
        with tempfile.TemporaryDirectory() as tmp:
            def build(directory):
                (directory / 'kernel.bin').write_bytes(b'kernel')
            builder = Mock(side_effect=build)
            first = prepare.boot.cached_component(Path(tmp), 'linux', {'version': 'old'}, ['kernel.bin'], builder)
            second = prepare.boot.cached_component(Path(tmp), 'linux', {'version': 'new'}, ['kernel.bin'], builder)
            self.assertNotEqual(first, second)
            (second / 'kernel.bin').write_bytes(b'corrupted')
            prepare.boot.cached_component(Path(tmp), 'linux', {'version': 'new'}, ['kernel.bin'], builder)
            self.assertEqual(builder.call_count, 3)

    def test_failed_build_is_not_cached(self):
        with tempfile.TemporaryDirectory() as tmp:
            builder = Mock(side_effect=RuntimeError('interrupted'))
            with self.assertRaises(RuntimeError):
                prepare.boot.cached_component(Path(tmp), 'linux', {}, ['kernel.bin'], builder)
            self.assertEqual(list(Path(tmp).rglob('manifest.json')), [])

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
