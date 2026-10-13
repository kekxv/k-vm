#!/usr/bin/env python3
"""Prepare a small, offline RISC-V Linux bundle from TinyEMU's reference image."""
import argparse
import os
import hashlib
import json
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path
from install_guest_bootstrap import install_bootstrap

URL = "https://bellard.org/tinyemu/diskimage-linux-riscv-2018-09-23.tar.gz"
SHA256 = "808ecc1b32efdd76103172129b77b46002a616dff2270664207c291e4fde9e14"
PREFIX = "diskimage-linux-riscv-2018-09-23/"
MAX_ARCHIVE = 32 * 1024 * 1024
MAX_ASSET = 128 * 1024 * 1024


def prepare(archive, output, arch, bootstrap=True):
    if output.exists():
        raise ValueError(f"output already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".k-vm-image-", dir=output.parent) as temporary:
        staging = Path(temporary) / "bundle"
        staging.mkdir()
        with tarfile.open(archive, "r:gz") as tar:
            for name in (f"bbl{arch}.bin", f"kernel-riscv{arch}.bin", f"root-riscv{arch}.bin"):
                member = tar.getmember(PREFIX + name)
                if not member.isfile() or not 0 < member.size <= MAX_ASSET:
                    raise ValueError(f"invalid boot asset: {name}")
                with tar.extractfile(member) as source, (staging / name).open("wb") as target:
                    shutil.copyfileobj(source, target)
        if bootstrap:
            install_bootstrap(staging / f"root-riscv{arch}.bin")
        config = {
            "version": 1, "machine": f"riscv{arch}", "memory_size": 128,
            "bios": f"bbl{arch}.bin", "kernel": f"kernel-riscv{arch}.bin",
            "cmdline": "console=hvc0 root=/dev/vda rw",
            "drive0": {"file": f"root-riscv{arch}.bin"},
        }
        (staging / "vm.cfg").write_text(json.dumps(config, indent=2) + "\n")
        (staging / "SOURCE.txt").write_text(
            "TinyEMU reference Linux image, 2018-09-23\n"
            f"Source: {URL}\nLinux and BusyBox retain their upstream licenses.\n"
        )
        # Do not extract archive paths, symlinks, permissions, or device nodes.
        staging.rename(output)
    print(f"Local image prepared: {output}")
    print(f"Run: ./bazel-bin/k-vm sandbox --image {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--no-bootstrap", action="store_true", help="legacy image without automatic workspace startup")
    parser.add_argument("--arch", choices=("32", "64"), default="64")
    parser.add_argument("--archive", type=Path, help="use a trusted local upstream archive, without downloading")
    args = parser.parse_args()
    output = (args.output or Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share") / "k-vm/images/reference-riscv64").absolute()
    try:
        if output.exists():
            raise ValueError(f"output already exists: {output}")
        if args.archive:
            prepare(args.archive, output, args.arch, not args.no_bootstrap)
        else:
            with tempfile.TemporaryDirectory(prefix="k-vm-download-") as temp:
                archive = Path(temp) / "image.tar.gz"
                digest = hashlib.sha256()
                size = 0
                with urllib.request.urlopen(URL, timeout=30) as source, archive.open("wb") as target:
                    while chunk := source.read(64 * 1024):
                        size += len(chunk)
                        if size > MAX_ARCHIVE:
                            raise ValueError("download exceeds expected archive size")
                        digest.update(chunk)
                        target.write(chunk)
                if digest.hexdigest() != SHA256:
                    raise ValueError("downloaded image SHA256 does not match the pinned archive")
                prepare(archive, output, args.arch, not args.no_bootstrap)
        return 0
    except (OSError, ValueError, KeyError, tarfile.TarError) as exc:
        print(f"prepare image: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
