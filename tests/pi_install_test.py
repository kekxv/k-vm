#!/usr/bin/env python3
"""Exercise installer validation without modifying the host runtime."""
import os
import hashlib
import tarfile
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'guest/k-vm-install-pi.sh'


class InstallerTest(unittest.TestCase):
    def test_local_install_verifies_archive_and_cleans_temporary_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            packages = root / 'packages'
            packages.mkdir()
            (packages / 'npm-cache').mkdir()
            prefix = root / 'prefix'
            distribution = root / 'node/bin'
            distribution.mkdir(parents=True)
            (distribution / 'node').write_text('#!/bin/sh\necho v24.21.0\n')
            (distribution / 'npm').write_text('''#!/bin/sh
set -eu
test -d "$npm_config_cache"
echo "$npm_config_cache" > "$K_VM_INSTALL_PREFIX/cache-path"
printf '#!/bin/sh\\necho 1.2.0\\n' > "$K_VM_INSTALL_PREFIX/bin/pi"
chmod +x "$K_VM_INSTALL_PREFIX/bin/pi"
''')
            for program in distribution.iterdir():
                program.chmod(0o755)
            name = 'node-v24.21.0-linux-riscv64.tar.xz'
            archive = packages / name
            with tarfile.open(archive, 'w:xz') as tar:
                tar.add(distribution.parent, arcname='node-v24.21.0-linux-riscv64')
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            (packages / 'SHASUMS256.txt').write_text(digest + '  ' + name + '\n')
            result = subprocess.run(['sh', str(SCRIPT), '--update', '--version', '1.2.0',
                                     '--offline', '--packages', str(packages)],
                                    env=dict(os.environ, K_VM_INSTALL_PREFIX=str(prefix)),
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((prefix / 'bin/pi').exists())
            self.assertFalse(Path((prefix / 'cache-path').read_text().strip()).exists())
            self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(), digest)

    def test_bad_checksum_never_extracts_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            packages = root / 'packages'
            packages.mkdir()
            name = 'node-v24.21.0-linux-riscv64.tar.xz'
            (packages / name).write_bytes(b'not an archive')
            (packages / 'SHASUMS256.txt').write_text('0' * 64 + '  ' + name + '\n')
            env = dict(os.environ, K_VM_INSTALL_PREFIX=str(root / 'prefix'))
            result = subprocess.run(['sh', str(SCRIPT), '--update', '--packages', str(packages)],
                                    env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('checksum', result.stderr.lower())
            self.assertFalse((root / 'prefix/bin/node').exists())

    def test_rejects_version_injection_before_install(self):
        result = subprocess.run(['sh', str(SCRIPT), '--version', '1.2.3;bad'],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('version', result.stderr)


if __name__ == '__main__':
    unittest.main()
