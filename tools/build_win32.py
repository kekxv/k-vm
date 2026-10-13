#!/usr/bin/env python3
"""Build native Win32 k-vm with MinGW-w64, on Linux or an MSYS2 Windows host."""
import argparse
import concurrent.futures
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
COMMON = [
    'src/core/cutils.c', 'src/core/iomem.c', 'src/core/json.c', 'src/core/machine.c',
    'src/arch/riscv/softfp.c', 'src/arch/riscv/riscv_machine.c',
    'src/devices/ide.c', 'src/devices/pci.c', 'src/devices/pckbd.c', 'src/devices/ps2.c',
    'src/devices/simplefb.c', 'src/devices/vga.c', 'src/devices/virtio.c', 'src/devices/vmmouse.c',
    'src/storage/fs.c', 'src/storage/fs_confined_win32.c', 'src/storage/win32_files.c',
    'src/storage/block_file_win32.c', 'src/storage/image_update_win32.c',
    'src/net/host_network.c', 'src/sandbox/launch.c', 'src/sandbox/workspace.c',
    'src/sandbox/workspace_host_win32.c', 'src/sandbox/update_win32.c',
    'src/frontend/main.c', 'src/frontend/console_win32.c', 'src/frontend/win32_entry.c',
]
LIBS = ['-lws2_32', '-liphlpapi', '-lshell32', '-lbcrypt', '-ladvapi32', '-lm']


def run(command):
    subprocess.run(command, cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC'))
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'out' / 'win32')
    parser.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument('--test', action='store_true', help='build and run native regression tests')
    parser.add_argument('--runner', help='runner for cross tests, e.g. /usr/lib/wine/wine64')
    args = parser.parse_args()
    cc = args.cc or ('gcc' if os.name == 'nt' else 'x86_64-w64-mingw32-gcc')
    if not shutil.which(cc):
        parser.error(f'{cc} not found; install MinGW-w64 or MSYS2 mingw-w64-x86_64-gcc')
    target = subprocess.check_output([cc, '-dumpmachine'], text=True).strip()
    if 'mingw' not in target or 'x86_64' not in target:
        parser.error(f'{cc} targets {target}, expected x86_64 MinGW-w64')
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    flags = ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-UNDEBUG', '-fno-strict-aliasing',
             '-D_GNU_SOURCE', '-D_FILE_OFFSET_BITS=64', '-D_LARGEFILE_SOURCE',
             '-D_WIN32_WINNT=0x0602', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX',
             '-mno-ms-bitfields',
             '-DCONFIG_VERSION="0.1.0"', '-DCONFIG_SLIRP', '-DCONFIG_RISCV_MAX_XLEN=128']
    flags += ['-I' + str(ROOT / p) for p in ['src/include', 'src/storage', 'src/sandbox', 'src/net', 'third_party']]
    sources = [(p, []) for p in COMMON]
    sources += [(str(p.relative_to(ROOT)), []) for p in sorted((ROOT / 'third_party/slirp').glob('*.c'))]
    sources += [('src/arch/riscv/riscv_cpu.c', ['-DMAX_XLEN=' + str(x)]) for x in (32, 64, 128)]
    def compile_one(item):
        source, extra = item
        suffix = ('_' + extra[0].split('=')[-1]) if extra else ''
        obj = out / (source.replace('/', '_').replace('.c', '') + suffix + '.o')
        run([cc, *flags, *extra, '-c', str(ROOT / source), '-o', str(obj)])
        return str(obj)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(compile_one, source) for source in sources]
        objects, failures = [], []
        for future in futures:
            try:
                objects.append(future.result())
            except subprocess.CalledProcessError as error:
                failures.append(error)
        if failures:
            raise failures[0]
    exe = out / 'k-vm.exe'
    run([cc, '-static', '-o', str(exe), *objects, *LIBS])
    print(exe)
    if args.test:
        tests = {
            'win32_disk_test': ['src/storage/block_file_win32.c', 'src/storage/image_update_win32.c'],
            'win32_filesystem_test': ['src/storage/fs_confined_win32.c'],
            'win32_workspace_test': ['src/sandbox/workspace_host_win32.c'],
            'win32_console_test': [],
            'network_test': ['src/net/host_network.c', 'src/core/cutils.c',
                             *[str(p.relative_to(ROOT)) for p in sorted((ROOT / 'third_party/slirp').glob('*.c'))]],
        }
        for name, modules in tests.items():
            test = out / (name + '.exe')
            run([cc, *flags, '-static', str(ROOT / 'tests' / (name + '.c')),
                 *[str(ROOT / p) for p in modules], str(ROOT / 'src/storage/win32_files.c'),
                 '-o', str(test), *LIBS])
            if args.runner:
                run([args.runner, str(test)])
            elif os.name == 'nt':
                run([str(test)])
            else:
                parser.error('--test on a non-Windows host needs --runner (Wine)')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
