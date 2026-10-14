#!/usr/bin/env python3
"""Package the RV64 base image with GNU sparse tar, xz and SHA256."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ASSETS = ('vm.cfg', 'sandbox.json', 'SOURCE.txt', 'kernel.config',
          'kernel.bin', 'opensbi.bin', 'rootfs.raw')
NAME = 'k-vm-base-riscv64'


def package(image, output):
    image = image.absolute()
    for name in ASSETS:
        file = image / name
        if file.is_symlink() or not file.is_file() or not file.stat().st_size:
            raise ValueError(f'missing or unsafe image asset: {file}')
    config = json.loads((image / 'vm.cfg').read_text())
    caps = json.loads((image / 'sandbox.json').read_text())
    if config.get('machine') != 'riscv64' or caps.get('agents') != ['shell'] or caps.get('pi_bootstrap') != 1:
        raise ValueError('release requires an RV64 base image with shell and on-demand Pi installation')
    if caps.get('requires_kvm') or 'node' in caps or 'pi_build_version' in caps:
        raise ValueError('base image must not require KVM or contain preinstalled Node.js/Pi')
    if (image / 'rootfs.raw').stat().st_size % 512:
        raise ValueError('root disk must be sector aligned')
    output = output.absolute()
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f'{NAME}.tar.xz'
    try:
        # Fixed filenames prevent temporary files, lock files or host config entering releases.
        subprocess.run(['tar', '--sparse', '--format=pax', '--sort=name',
                        '--owner=0', '--group=0', '--numeric-owner',
                        '--use-compress-program=xz -T2 -6',
                        '--transform', f's,^,{NAME}/,',
                        '-cf', str(archive), '-C', str(image), *ASSETS], check=True)
        with archive.open('rb') as source:
            checksum = hashlib.file_digest(source, 'sha256').hexdigest()
        Path(str(archive) + '.sha256').write_text(f'{checksum}  {archive.name}\n')
    except Exception:
        archive.unlink(missing_ok=True)
        Path(str(archive) + '.sha256').unlink(missing_ok=True)
        raise
    print(f'Base image archive ready: {archive} ({archive.stat().st_size / 1024**2:.1f} MiB)')
    return archive


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path('dist'))
    args = parser.parse_args()
    package(args.image, args.output)
