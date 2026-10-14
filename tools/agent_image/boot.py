"""Pinned RV64 boot builds with independently verified component caches."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import urllib.request

VERSIONS = Path(__file__).with_name('versions.json')


def load_versions(path=None):
    versions = json.loads(Path(path or VERSIONS).read_text())
    for component, pattern in [('linux', r'\d+\.\d+\.\d+'), ('opensbi', r'\d+\.\d+(?:\.\d+)?')]:
        release = versions[component]
        if not re.fullmatch(pattern, release['version']) or not re.fullmatch(r'[a-f0-9]{64}', release['sha256']):
            raise ValueError(f'invalid pinned {component} version/checksum')
    return versions


def compiler_identity():
    return subprocess.check_output(['riscv64-linux-gnu-gcc', '--version'], text=True).strip()


def signature(release, compiler):
    return {'release': release, 'compiler': compiler,
            'recipe': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}


def cache_key(versions):
    inputs = signature(versions, compiler_identity())
    return hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def cached_component(cache, component, inputs, assets, builder):
    key = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    parent = cache / 'boot' / component
    parent.mkdir(parents=True, exist_ok=True)
    output = parent / key
    # Different recipes/compilers can share the same extracted source tree.
    with (parent / '.build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            manifest = json.loads((output / 'manifest.json').read_text())
            valid = manifest['inputs'] == inputs and all(
                (output / name).is_file() and not (output / name).is_symlink() and
                (output / name).stat().st_size > 0 and digest(output / name) == manifest['sha256'][name]
                for name in assets)
        except (OSError, ValueError, KeyError, TypeError):
            valid = False
        if valid:
            print(f'Boot cache hit: {component}', flush=True)
            return output
        print(f'Boot cache miss: {component}', flush=True)
        with tempfile.TemporaryDirectory(prefix='.build-', dir=parent) as tmp:
            bundle = Path(tmp) / 'bundle'
            bundle.mkdir()
            builder(bundle)
            for name in assets:
                if not (bundle / name).is_file() or (bundle / name).is_symlink() or not (bundle / name).stat().st_size:
                    raise ValueError(f'missing built asset: {name}')
            (bundle / 'manifest.json').write_text(json.dumps({
                'inputs': inputs, 'sha256': {name: digest(bundle / name) for name in assets}}, sort_keys=True) + '\n')
            if output.exists():
                shutil.rmtree(output)
            bundle.rename(output)
    return output


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


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


def _build_kernel(cache, release, output):
    version = release['version']
    name = f'linux-{version}.tar.xz'
    checked_download(f'https://cdn.kernel.org/pub/linux/kernel/v{version.split(".")[0]}.x/{name}',
                     cache / name, release['sha256'])
    kernel = cache / f'linux-{version}'
    if not kernel.exists():
        run('tar', '-xf', str(cache / name), '-C', str(cache))
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
    shutil.copyfile(kernel / 'arch/riscv/boot/Image', output / 'kernel.bin')
    shutil.copyfile(kernel / '.config', output / 'kernel.config')


def _build_opensbi(cache, release, output):
    version = release['version']
    name = f'opensbi-v{version}.tar.gz'
    checked_download(f'https://github.com/riscv-software-src/opensbi/archive/refs/tags/v{version}.tar.gz',
                     cache / name, release['sha256'])
    firmware = cache / f'opensbi-{version}'
    if not firmware.exists():
        run('tar', '-xf', str(cache / name), '-C', str(cache))
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
    shutil.copyfile(firmware / 'build/platform/generic/firmware/fw_jump.bin', output / 'opensbi.bin')


def build_boot(cache, versions):
    cache.mkdir(parents=True, exist_ok=True)
    compiler = compiler_identity()
    linux = cached_component(cache, 'linux', signature(versions['linux'], compiler),
        ['kernel.bin', 'kernel.config'], lambda output: _build_kernel(cache, versions['linux'], output))
    firmware = cached_component(cache, 'opensbi', signature(versions['opensbi'], compiler),
        ['opensbi.bin'], lambda output: _build_opensbi(cache, versions['opensbi'], output))
    return linux / 'kernel.bin', firmware / 'opensbi.bin', versions['linux']['sha256']
