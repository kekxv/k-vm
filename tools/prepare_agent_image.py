#!/usr/bin/env python3
"""Build a Debian slim RV64 Pi image for TinyEMU (no KVM required)."""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import urllib.request
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NODE_VERSION = '24.21.0'
PI_VERSION = '1.2.0'
OPENSBI_VERSION = '1.9'
OPENSBI_SHA256 = 'f6c4985843d6d878b5b511d7d7a930b6b541d3d9305371f2d9780765428f66f4'


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def stable_kernel(releases):
    return next(item['version'] for item in releases['releases'] if item['moniker'] == 'stable')


def vm_config():
    return {'version': 1, 'machine': 'riscv64', 'memory_size': 1024,
            'bios': 'opensbi.bin', 'kernel': 'kernel.bin',
            'cmdline': 'earlycon=sbi console=hvc0 root=/dev/vda rw init=/sbin/k-vm-init',
            'drive0': {'file': 'rootfs.raw'}}


def fetch(url):
    with urllib.request.urlopen(url, timeout=120) as response:
        return response.read()


def checked_download(url, destination, checksum):
    if not destination.exists():
        partial = destination.with_suffix(destination.suffix + '.partial')
        partial.write_bytes(fetch(url))
        partial.rename(destination)
    with destination.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    if digest != checksum:
        raise ValueError(f'SHA256 mismatch for {destination}; remove it and retry')


def published_checksum(url, filename):
    for line in fetch(url).decode().splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1] == filename and re.fullmatch('[a-f0-9]{64}', parts[0]):
            return parts[0]
    raise ValueError(f'publisher checksum missing for {filename}')


def build_boot(cache, kernel_version):
    kernel_name = f'linux-{kernel_version}.tar.xz'
    kernel_base = f'https://cdn.kernel.org/pub/linux/kernel/v{kernel_version.split(".")[0]}.x/'
    kernel_sha = published_checksum(kernel_base + 'sha256sums.asc', kernel_name)
    checked_download(kernel_base + kernel_name, cache / kernel_name, kernel_sha)
    firmware_name = f'opensbi-v{OPENSBI_VERSION}.tar.gz'
    checked_download(f'https://github.com/riscv-software-src/opensbi/archive/refs/tags/v{OPENSBI_VERSION}.tar.gz',
                     cache / firmware_name, OPENSBI_SHA256)
    kernel = cache / f'linux-{kernel_version}'
    firmware = cache / f'opensbi-{OPENSBI_VERSION}'
    for archive, extracted in ((cache / kernel_name, kernel), (cache / firmware_name, firmware)):
        if not extracted.exists():
            run('tar', '-xf', str(archive), '-C', str(cache))
    make = ('make', 'ARCH=riscv', 'CROSS_COMPILE=riscv64-linux-gnu-')
    run(*make, 'defconfig', cwd=kernel)
    disable = ('RISCV_ISA_V', 'RISCV_ISA_ZBB', 'RISCV_ISA_ZBA', 'RISCV_ISA_ZBS',
               'RISCV_ISA_ZICBOM', 'RISCV_ISA_ZICBOZ', 'RISCV_ISA_ZIHINTPAUSE',
               'SMP', 'RANDOMIZE_BASE', 'MODULES', 'DRM', 'SOUND', 'WLAN',
               'USB_SUPPORT', 'PCI', 'SCSI', 'SATA_AHCI', 'NVME_CORE',
               'DEBUG_INFO', 'DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT',
               'ARCH_ANDES', 'ARCH_ANLOGIC', 'ARCH_ESWIN', 'ARCH_MICROCHIP',
               'ARCH_SIFIVE', 'ARCH_SOPHGO', 'ARCH_SPACEMIT', 'SOC_STARFIVE',
               'ARCH_SUNXI', 'ARCH_TENSTORRENT', 'ARCH_THEAD', 'ARCH_ULTRARISC',
               'ARCH_CANAAN', 'ARCH_RENESAS', 'ACPI', 'VIRTUALIZATION', 'KVM',
               'IOMMU_SUPPORT', 'CPU_FREQ', 'CPU_IDLE', 'PM', 'RISCV_SBI_CPUIDLE',
               'SERIAL_8250', 'RISCV_SBI_MPXY', 'HVC_RISCV_SBI')
    enable = ('NONPORTABLE', 'SERIAL_EARLYCON_RISCV_SBI', 'NET_9P', 'NET_9P_VIRTIO', '9P_FS', 'VIRTIO_MMIO',
              'VIRTIO_BLK', 'VIRTIO_NET', 'DEVTMPFS_MOUNT', 'EXT4_FS', 'RISCV_SBI_V01')
    options = [value for name in disable for value in ('--disable', name)]
    options += [value for name in enable for value in ('--enable', name)]
    run(str(kernel / 'scripts/config'), *options, cwd=kernel)
    run(*make, 'olddefconfig', cwd=kernel)
    run(*make, f'-j{os.cpu_count() or 2}', 'Image', cwd=kernel)
    # OpenSBI 1.9's custom HTIF pointers lack volatile: GCC can remove the
    # shutdown MMIO writes and compile htif_system_reset into an endless loop.
    htif = firmware / 'lib/utils/sys/htif.c'
    source = htif.read_text()
    source = source.replace('static uint64_t *htif_', 'static volatile uint64_t *htif_')
    source = source.replace('(uint64_t *)custom_', '(volatile uint64_t *)custom_')
    htif.write_text(source)
    run('make', f'-j{os.cpu_count() or 2}', 'CROSS_COMPILE=riscv64-linux-gnu-',
        'PLATFORM=generic', 'PLATFORM_RISCV_ISA=rv64imafdc_zicsr_zifencei',
        'FW_JUMP_ADDR=0x80200000', cwd=firmware)
    return kernel / 'arch/riscv/boot/Image', firmware / 'build/platform/generic/firmware/fw_jump.bin', kernel_sha


def prepare(output, size_gb=4, cache=None, kernel_version=None):
    output = output.absolute()
    if output.exists():
        raise ValueError(f'output already exists: {output}')
    for tool in ('docker', 'tar', 'mke2fs', 'make', 'riscv64-linux-gnu-gcc', 'dtc'):
        if not shutil.which(tool):
            raise ValueError(f'required host tool missing: {tool}')
    cache = (cache or Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'k-vm/riscv-build').absolute()
    cache.mkdir(parents=True, exist_ok=True)
    if kernel_version is None:
        kernel_version = stable_kernel(json.loads(fetch('https://www.kernel.org/releases.json')))
    if not re.fullmatch(r'\d+\.\d+\.\d+', kernel_version):
        raise ValueError('kernel version must be a stable x.y.z release')
    kernel, firmware, kernel_sha = build_boot(cache, kernel_version)
    node_name = f'node-v{NODE_VERSION}-linux-riscv64.tar.xz'
    node_base = f'https://unofficial-builds.nodejs.org/download/release/v{NODE_VERSION}/'
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
        shutil.copyfile(cache / node_name, context / 'node-riscv64.tar.xz')
        bundle = temp / 'bundle'
        bundle.mkdir()
        rootfs = temp / 'rootfs'
        rootfs.mkdir()
        try:
            # Only these three public build inputs enter Docker; no project or credentials.
            run('docker', 'build', '--platform', 'linux/riscv64',
                '--build-arg', 'HTTP_PROXY', '--build-arg', 'HTTPS_PROXY',
                '--build-arg', 'NO_PROXY', '-t', tag, str(context))
            container = run('docker', 'create', '--platform', 'linux/riscv64', tag, text=True, capture_output=True).stdout.strip()
            archive = temp / 'rootfs.tar'
            run('docker', 'export', '--output', str(archive), container)
            run('tar', '--extract', '--file', str(archive), '--directory', str(rootfs), '--numeric-owner')
            shutil.copyfile(kernel, bundle / 'kernel.bin')
            shutil.copyfile(firmware, bundle / 'opensbi.bin')
            shutil.copyfile(cache / f'linux-{kernel_version}/.config', bundle / 'kernel.config')
            disk = bundle / 'rootfs.raw'
            with disk.open('wb') as target:
                target.truncate(size_gb * 1024**3)
            run('mke2fs', '-q', '-t', 'ext4', '-F', '-d', str(rootfs), str(disk))
            (bundle / 'vm.cfg').write_text(json.dumps(vm_config(), indent=2) + '\n')
            (bundle / 'sandbox.json').write_text(json.dumps({
                'bootstrap': 1, 'arch': 'riscv64', 'agents': ['shell', 'pi'], 'requires_kvm': False,
                'kernel': kernel_version, 'kernel_sha256': kernel_sha, 'opensbi': OPENSBI_VERSION,
                'opensbi_sha256': OPENSBI_SHA256, 'node': NODE_VERSION, 'node_sha256': node_sha,
                'opensbi_patches': ['htif-volatile-mmio'],
                'pi_build_version': PI_VERSION, 'distribution': 'Debian trixie-slim',
            }, indent=2) + '\n')
            (bundle / 'SOURCE.txt').write_text(
                f'Linux {kernel_version}: {kernel_sha}\nOpenSBI {OPENSBI_VERSION}: {OPENSBI_SHA256}\n'
                f'Debian trixie-slim RV64; community Node {NODE_VERSION}: {node_sha}; Pi {PI_VERSION}.\n'
                'OpenSBI custom HTIF MMIO pointers patched volatile for reliable shutdown.\n'
                'Built from tools/agent_image/Dockerfile. Third-party components retain their licenses.\n')
            bundle.rename(output)
        finally:
            if container:
                subprocess.run(['docker', 'rm', container], check=False, stdout=subprocess.DEVNULL)
            subprocess.run(['docker', 'image', 'rm', tag], check=False, stdout=subprocess.DEVNULL)
    print(f'Pi RV64 image ready: {output}')
    print(f'K_VM_IMAGE={output} k-vm sandbox --agent pi')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share') / 'k-vm/images/pi-riscv64')
    parser.add_argument('--size-gb', type=int, choices=range(3, 17), default=4)
    parser.add_argument('--build-cache', type=Path)
    parser.add_argument('--kernel-version', help='pin x.y.z; default: current stable from kernel.org')
    args = parser.parse_args()
    prepare(args.output, args.size_gb, args.build_cache, args.kernel_version)
