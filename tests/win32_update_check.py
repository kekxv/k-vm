#!/usr/bin/env python3
"""Prepare/run a native Windows guest update fixture; no npm downloads needed."""
import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path
from win32_guest_check import Guest


def prepare(source, destination):
    shutil.copytree(source, destination)
    disk = destination / 'root-riscv64.bin'
    (destination / 'sandbox.json').write_text(json.dumps({'bootstrap': 1, 'agents': ['shell', 'pi']}))
    scripts = {
        'npm': '''#!/bin/sh
set -eu
test "$1" = install && test "$2" = -g
test ! -e /root/.pi/agent/auth.json
if grep -q ' work ' /proc/mounts; then exit 90; fi
case "$3" in
  *@fail) echo partial > /root/pi-version; exit 42;;
  *@slow) echo partial > /root/pi-version; echo UPDATE_SLEEPING; sleep 60;;
  *) printf '%s\\n' "$3" > /root/pi-version;;
esac
''',
        'pi': '#!/bin/sh\ncat /root/pi-version\n',
    }
    with tempfile.TemporaryDirectory() as temporary:
        for name, script in scripts.items():
            local = Path(temporary) / name
            local.write_bytes(script.encode())
            subprocess.run(['debugfs', '-w', '-f', '-', str(disk.resolve())],
                           input=f'write {name} /usr/bin/{name}\nset_inode_field /usr/bin/{name} mode 0100755\n',
                           text=True, check=True, cwd=temporary, capture_output=True)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').digest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--prepare', type=Path, help='Create fixture directory using Linux debugfs, then exit')
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--wine')
    args = parser.parse_args()
    if args.prepare:
        prepare(args.image, args.prepare)
        return
    if not args.binary:
        parser.error('--binary required for execution')
    with tempfile.TemporaryDirectory(prefix='k-vm-update-native-') as temporary:
        image = Path(temporary) / 'image'
        shutil.copytree(args.image, image)
        path = str(image.resolve())
        if args.wine:
            path = 'Z:' + path.replace('/', '\\')
        prefix = [args.wine] if args.wine else []
        command = prefix + [str(args.binary.resolve())]
        disk = image / 'root-riscv64.bin'
        before = digest(disk)
        for version, success in [('1.2.3', True), ('fail', False), ('slow', False)]:
            guest = Guest(command + ['update', '--image', path, '--version', version], temporary)
            try:
                if version == 'slow':
                    guest.until(b'UPDATE_SLEEPING\r\n')
                    guest.send(b'\x01x')
                status = guest.proc.wait(90)
                assert (status == 0) == success, (version, status, bytes(guest.output))
                current = digest(disk)
                assert (current != before) == success, version
                assert not list(image.glob('*.update-*')), 'staging image leaked'
                before = current
            finally:
                guest.close()
        guest = Guest(command + ['sandbox', '--image', path, '--agent', 'shell'], temporary)
        try:
            guest.until(b'/work # ')
            result = guest.command('cat /root/pi-version')
            assert b'@earendil-works/pi-coding-agent@1.2.3\r\n' in result, result
            guest.send(b'exit\n')
            assert guest.proc.wait(30) == 0
        finally:
            guest.close()
        print('Native guest update: committed version, no work/auth, failed/cancelled updates preserve disk, staging removed')


if __name__ == '__main__':
    main()
