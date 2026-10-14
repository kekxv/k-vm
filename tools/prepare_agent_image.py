#!/usr/bin/env python3
"""Build a Debian slim RV64 base image; optionally preinstall Node.js/Pi."""
import argparse
import json
import os
import re
import shutil
import subprocess
import tempfile
import uuid
from pathlib import Path
from agent_image import boot

ROOT = Path(__file__).resolve().parent.parent
NODE_VERSION = '24.21.0'
PI_VERSION = '1.2.0'
fetch = boot.fetch
checked_download = boot.checked_download
published_checksum = boot.published_checksum


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def stable_kernel(releases):
    return next(item['version'] for item in releases['releases'] if item['moniker'] == 'stable')


def vm_config():
    return {'version': 1, 'machine': 'riscv64', 'memory_size': 1024,
            'bios': 'opensbi.bin', 'kernel': 'kernel.bin',
            'cmdline': 'earlycon=sbi console=hvc0 root=/dev/vda rw init=/sbin/k-vm-init',
            'drive0': {'file': 'rootfs.raw'}}


def agent_capabilities(with_pi):
    return {'bootstrap': 1, 'arch': 'riscv64', 'agents': ['shell', 'pi'] if with_pi else ['shell'],
            'pi_bootstrap': 1, 'requires_kvm': False}


def prepare(output, size_gb=4, cache=None, kernel_version=None, with_pi=False, versions_file=None):
    output = output.absolute()
    if output.exists():
        raise ValueError(f'output already exists: {output}')
    for tool in ('docker', 'tar', 'mke2fs', 'make', 'riscv64-linux-gnu-gcc', 'dtc'):
        if not shutil.which(tool):
            raise ValueError(f'required host tool missing: {tool}')
    cache = (cache or Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'k-vm/riscv-build').absolute()
    cache.mkdir(parents=True, exist_ok=True)
    versions = boot.load_versions(versions_file)
    if kernel_version == 'latest':
        kernel_version = stable_kernel(json.loads(fetch('https://www.kernel.org/releases.json')))
    if kernel_version is None:
        kernel_version = versions['linux']['version']
    if not re.fullmatch(r'\d+\.\d+\.\d+', kernel_version):
        raise ValueError('kernel version must be a stable x.y.z release')
    if kernel_version != versions['linux']['version']:
        name = f'linux-{kernel_version}.tar.xz'
        base = f'https://cdn.kernel.org/pub/linux/kernel/v{kernel_version.split(".")[0]}.x/'
        versions['linux'] = {'version': kernel_version, 'sha256': published_checksum(base + 'sha256sums.asc', name)}
    kernel, firmware, kernel_sha = boot.build_boot(cache, versions)
    opensbi_version = versions['opensbi']['version']
    opensbi_sha = versions['opensbi']['sha256']
    node_name = f'node-v{NODE_VERSION}-linux-riscv64.tar.xz'
    node_base = f'https://unofficial-builds.nodejs.org/download/release/v{NODE_VERSION}/'
    node_sha = None
    if with_pi:
        node_sha = published_checksum(node_base + 'SHASUMS256.txt', node_name)
        checked_download(node_base + node_name, cache / node_name, node_sha)
    output.parent.mkdir(parents=True, exist_ok=True)
    tag = 'k-vm-pi-build:' + uuid.uuid4().hex
    container = None
    with tempfile.TemporaryDirectory(prefix='.k-vm-pi-', dir=output.parent) as temporary:
        temp = Path(temporary)
        context = temp / 'context'
        context.mkdir()
        shutil.copyfile(ROOT / 'tools/agent_image/Dockerfile', context / 'Dockerfile')
        shutil.copyfile(ROOT / 'guest/k-vm-init.sh', context / 'k-vm-init.sh')
        shutil.copyfile(ROOT / 'guest/k-vm-install-pi.sh', context / 'k-vm-install-pi.sh')
        if with_pi:
            shutil.copyfile(cache / node_name, context / 'node-riscv64.tar.xz')
        bundle = temp / 'bundle'
        bundle.mkdir()
        rootfs = temp / 'rootfs'
        rootfs.mkdir()
        try:
            # Only public build inputs enter Docker; no project or credentials.
            run('docker', 'build', '--platform', 'linux/riscv64',
                '--target', 'pi' if with_pi else 'base',
                '--build-arg', f'PI_VERSION={PI_VERSION}',
                '--build-arg', 'HTTP_PROXY', '--build-arg', 'HTTPS_PROXY',
                '--build-arg', 'NO_PROXY', '-t', tag, str(context))
            container = run('docker', 'create', '--platform', 'linux/riscv64', tag, text=True, capture_output=True).stdout.strip()
            archive = temp / 'rootfs.tar'
            run('docker', 'export', '--output', str(archive), container)
            run('tar', '--extract', '--file', str(archive), '--directory', str(rootfs), '--numeric-owner')
            shutil.copyfile(kernel, bundle / 'kernel.bin')
            shutil.copyfile(firmware, bundle / 'opensbi.bin')
            shutil.copyfile(kernel.parent / 'kernel.config', bundle / 'kernel.config')
            disk = bundle / 'rootfs.raw'
            with disk.open('wb') as target:
                target.truncate(size_gb * 1024**3)
            run('mke2fs', '-q', '-t', 'ext4', '-F', '-d', str(rootfs), str(disk))
            (bundle / 'vm.cfg').write_text(json.dumps(vm_config(), indent=2) + '\n')
            (bundle / 'sandbox.json').write_text(json.dumps({
                **agent_capabilities(with_pi),
                'kernel': kernel_version, 'kernel_sha256': kernel_sha, 'opensbi': opensbi_version,
                'opensbi_sha256': opensbi_sha,
                **({'node': NODE_VERSION, 'node_sha256': node_sha, 'pi_build_version': PI_VERSION} if with_pi else {}),
                'node_install_version': NODE_VERSION,
                'opensbi_patches': ['htif-volatile-mmio'],
                'distribution': 'Debian trixie-slim',
            }, indent=2) + '\n')
            (bundle / 'SOURCE.txt').write_text(
                f'Linux {kernel_version}: {kernel_sha}\nOpenSBI {opensbi_version}: {opensbi_sha}\n'
                'Debian trixie-slim RV64 with on-demand Node/Pi installer.\n'
                + (f'Community Node {NODE_VERSION}: {node_sha}; Pi {PI_VERSION}.\n' if with_pi else 'Node.js and Pi are not preinstalled.\n') +
                'OpenSBI custom HTIF MMIO pointers patched volatile for reliable shutdown.\n'
                'Built from tools/agent_image/Dockerfile. Third-party components retain their licenses.\n')
            bundle.rename(output)
        finally:
            if container:
                subprocess.run(['docker', 'rm', container], check=False, stdout=subprocess.DEVNULL)
            subprocess.run(['docker', 'image', 'rm', tag], check=False, stdout=subprocess.DEVNULL)
    print(f'{"Pi" if with_pi else "Base"} RV64 image ready: {output}')
    if not with_pi:
        print(f'k-vm init --image {output} --agent pi  # optional: install ahead of first use')
    print(f'K_VM_IMAGE={output} k-vm sandbox --agent pi')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share') / 'k-vm/images/pi-riscv64')
    parser.add_argument('--size-gb', type=int, choices=range(3, 17), default=4)
    parser.add_argument('--build-cache', type=Path)
    parser.add_argument('--kernel-version', help='override pinned version with x.y.z or latest')
    parser.add_argument('--versions', type=Path, help='boot version/checksum JSON (default: tools/agent_image/versions.json)')
    parser.add_argument('--print-boot-cache-key', action='store_true', help='print CI cache key without preparing an image')
    parser.add_argument('--with-pi', action='store_true', help='preinstall Node.js and Pi (default: base shell image)')
    args = parser.parse_args()
    if args.print_boot_cache_key:
        print(boot.cache_key(boot.load_versions(args.versions)))
    else:
        prepare(args.output, args.size_gb, args.build_cache, args.kernel_version, args.with_pi, args.versions)
