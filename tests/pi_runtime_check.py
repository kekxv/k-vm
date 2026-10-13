#!/usr/bin/env python3
"""Verify real Pi, Node and HTTPS in a prepared RV64 image (requires network)."""
import argparse
import os
import pty
import re
import subprocess
import tempfile
from pathlib import Path
from agent_workspace_check import wait_for_prompt


def check(binary, image):
    with tempfile.TemporaryDirectory(prefix="k-vm-pi-runtime-") as temporary:
        work = Path(temporary) / "work"
        config = Path(temporary) / "config"
        work.mkdir()
        config.mkdir()
        master, slave = pty.openpty()
        proc = subprocess.Popen(
            [binary, "sandbox", "--image", image, "--agent", "pi",
             "--config-dir", config, "--", "--version"],
            cwd=work, stdin=slave, stdout=slave, stderr=slave,
        )
        try:
            output = wait_for_prompt(proc, master, timeout=120)
            assert re.search(rb"\n\d+\.\d+\.\d+\r", output), output
            assert b"agent ended (0)" in output, output
            commands = {
                "node --version > node.txt": "node.txt",
                "uname -r > kernel.txt": "kernel.txt",
                "curl -fsS --max-time 30 https://registry.npmjs.org/ > registry.json": "registry.json",
            }
            for command, name in commands.items():
                os.write(master, (command + "\n").encode())
                wait_for_prompt(proc, master, timeout=90)
                assert (work / name).read_bytes(), name
            assert (work / "node.txt").read_text().startswith("v")
            os.write(master, b"exit\n")
            assert proc.wait(10) == 0
            print("Real RV64 Pi version, Node, kernel, /work and npm registry HTTPS verified")
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
            os.close(slave)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("bazel-bin/k-vm"))
    parser.add_argument("--image", type=Path, default=Path("images/pi-riscv64"))
    args = parser.parse_args()
    check(args.binary.resolve(), args.image.resolve())
