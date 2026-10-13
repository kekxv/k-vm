#!/usr/bin/env python3
"""Install k-vm's PID1 bootstrap into an ext2/ext4 raw image using debugfs."""
import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "guest" / "k-vm-init.sh"


def install_bootstrap(image):
    # debugfs rm does not follow a merged-/usr directory symlink safely.
    directory = subprocess.run(
        ["debugfs", "-R", "stat /sbin", str(Path(image).resolve())],
        capture_output=True, text=True, check=True,
    )
    target = "/usr/sbin/k-vm-init" if "Type: symlink" in directory.stdout else "/sbin/k-vm-init"
    # debugfs command syntax has no reliable arbitrary-path quoting. Use a fixed
    # temporary filename and an absolute image argument, never interpolated code.
    with tempfile.TemporaryDirectory(prefix="k-vm-bootstrap-") as temporary:
        local = Path(temporary) / "k-vm-init"
        local.write_bytes(SCRIPT.read_bytes())
        local.chmod(0o755)
        commands = f"rm {target}\nwrite k-vm-init {target}\nset_inode_field {target} mode 0100755\n"
        result = subprocess.run(
            ["debugfs", "-w", "-f", "-", str(Path(image).resolve())],
            input=commands, text=True, capture_output=True, cwd=temporary, check=True,
        )
        check = subprocess.run(
            ["debugfs", "-R", "cat /sbin/k-vm-init", str(Path(image).resolve())],
            capture_output=True, check=True,
        )
        if check.stdout != local.read_bytes():
            raise RuntimeError("unable to install guest bootstrap: " + result.stderr)
    image = Path(image).resolve()
    (image.parent / "sandbox.json").write_text(json.dumps({
        "bootstrap": 1, "agents": ["shell"], "requires_kvm": False,
    }, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    install_bootstrap(args.image)
