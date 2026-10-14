#!/usr/bin/env python3
"""Release image archives preserve sparse disks and include only boot assets."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('package_image', Path(__file__).parents[1] / 'tools/package_image.py')
package_image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package_image)


class ImagePackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.image = self.root / 'base'
        self.image.mkdir()
        for name in ('kernel.bin', 'opensbi.bin', 'kernel.config', 'SOURCE.txt'):
            (self.image / name).write_text(name + '\n')
        (self.image / 'vm.cfg').write_text(json.dumps({'machine': 'riscv64'}))
        (self.image / 'sandbox.json').write_text(json.dumps({'agents': ['shell'], 'pi_bootstrap': 1}))
        with (self.image / 'rootfs.raw').open('wb') as disk:
            disk.write(b'first block')
            disk.seek(64 * 1024**2 - 4)
            disk.write(b'last')

    def test_roundtrip_sparse_disk_excludes_adjacent_credentials_and_locks(self):
        (self.image / 'auth.json').write_text('secret')
        (self.image / 'rootfs.raw.lock').write_text('lock')
        archive = package_image.package(self.image, self.root / 'dist')
        self.assertLess(archive.stat().st_size, 1024 * 1024)
        with tarfile.open(archive) as tar:
            names = tar.getnames()
        self.assertNotIn('k-vm-base-riscv64/auth.json', names)
        self.assertNotIn('k-vm-base-riscv64/rootfs.raw.lock', names)
        extracted = self.root / 'extracted'
        extracted.mkdir()
        subprocess.run(['tar', '-xf', str(archive), '-C', str(extracted)], check=True)
        restored = extracted / 'k-vm-base-riscv64/rootfs.raw'
        self.assertEqual(restored.stat().st_size, 64 * 1024**2)
        with restored.open('rb') as disk:
            self.assertEqual(disk.read(11), b'first block')
            disk.seek(-4, 2)
            self.assertEqual(disk.read(), b'last')
        checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
        self.assertEqual(Path(str(archive) + '.sha256').read_text(), f'{checksum}  {archive.name}\n')

    def test_rejects_full_pi_image_before_creating_archive(self):
        (self.image / 'sandbox.json').write_text(json.dumps({'agents': ['shell', 'pi'], 'pi_bootstrap': 1}))
        with self.assertRaises(ValueError):
            package_image.package(self.image, self.root / 'dist')
        self.assertFalse((self.root / 'dist').exists())


if __name__ == '__main__':
    unittest.main()
