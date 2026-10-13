#!/usr/bin/env python3
"""Validate Linux release ABI and package a standalone k-vm artifact."""
import argparse
import hashlib
import re
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def inspect(binary, arch, libc, glibc):
    def readelf(*options):
        return subprocess.check_output(["readelf", *options, str(binary)], text=True)
    header = readelf("-h")
    expected = "Advanced Micro Devices X86-64" if arch == "amd64" else "AArch64"
    if expected not in header:
        raise ValueError(f"expected {arch} ELF")
    program = readelf("-l")
    dynamic = readelf("-d")
    if libc == "musl":
        if "INTERP" in program or "(NEEDED)" in dynamic:
            raise ValueError("musl release must be static, without interpreter or shared libraries")
    else:
        versions = re.findall(r"GLIBC_(\d+)\.(\d+)", readelf("--version-info"))
        ceiling = tuple(map(int, glibc.split(".")))
        if not versions or max(tuple(map(int, v)) for v in versions) > ceiling:
            raise ValueError(f"binary requires glibc newer than {glibc} or has no glibc version metadata")
        needed = re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic)
        allowed = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1"}
        if set(needed) - allowed:
            raise ValueError(f"unexpected shared dependency: {needed}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "bazel-bin/k-vm")
    parser.add_argument("--arch", choices=("amd64", "arm64"), required=True)
    parser.add_argument("--libc", choices=("glibc", "musl"), required=True)
    parser.add_argument("--glibc", default="2.28")
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    args = parser.parse_args()
    inspect(args.binary, args.arch, args.libc, args.glibc)
    suffix = "musl-static" if args.libc == "musl" else "glibc-" + args.glibc
    name = f"k-vm-linux-{args.arch}-{suffix}"
    args.output.mkdir(parents=True, exist_ok=True)
    archive = args.output / (name + ".tar.gz")
    with tempfile.TemporaryDirectory() as temporary:
        bundle = Path(temporary) / name
        bundle.mkdir()
        shutil.copy2(args.binary, bundle / "k-vm")
        (bundle / "k-vm").chmod(0o755)
        shutil.copy2(ROOT / "README.md", bundle / "README.md")
        shutil.copy2(ROOT / "LICENSE", bundle / "LICENSE")
        (bundle / "docs").mkdir()
        for guide in ("usage.md", "build-release.md"):
            shutil.copy2(ROOT / "docs" / guide, bundle / "docs" / guide)
        shutil.copytree(ROOT / "examples", bundle / "examples")
        # Upstream license notices are embedded in individual sources.
        (bundle / "licenses").mkdir()
        notices = "\n\n".join(p.name + "\n" + p.read_text().split("*/", 1)[0] + "*/"
                              for p in sorted((ROOT / "third_party/slirp").iterdir())
                              if p.suffix in (".c", ".h") and p.read_text().startswith("/*"))
        (bundle / "licenses/SLIRP-NOTICES.txt").write_text(notices)
        for notice in (ROOT / "third_party/licenses").glob("*.txt"):
            shutil.copy2(notice, bundle / "licenses" / notice.name)
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(bundle, arcname=name)
    checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(archive.suffix + ".sha256").write_text(f"{checksum}  {archive.name}\n")
    print(f"Validated {args.arch}/{suffix}: {archive}")


if __name__ == "__main__":
    main()
