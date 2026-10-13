#!/usr/bin/env python3
"""Validate dispatch/config isolation using fixture programs, not real agents."""
import json
import os
import pty
import shutil
import subprocess
import tempfile
from pathlib import Path
from agent_workspace_check import wait_for_prompt


def check(binary, source):
    with tempfile.TemporaryDirectory(prefix="k-vm-dispatch-") as temporary:
        root = Path(temporary)
        image = root / "image"
        shutil.copytree(source, image)
        disk = image / "root-riscv64.bin"
        (image / "sandbox.json").write_text(json.dumps({"agents": ["shell", "pi"]}))
        fixture = root / "fixture.sh"
        fixture.write_text('''#!/bin/sh
set -eu
name=${0##*/}
test "$PWD" = /work
config=$PI_CODING_AGENT_DIR
test -f "$config/auth.json"
test ! -e "$config/history.jsonl"
test ! -e "$config/sessions"
ls "$config" > config-files.txt
printf '%s\\n' "$name" "$@" > dispatch.txt
''')
        fixture.chmod(0o755)
        for agent in ("pi",):
            subprocess.run(["debugfs", "-w", "-R", f"write {fixture} /usr/bin/{agent}", disk],
                           check=True, capture_output=True)
        config = root / "config"
        config.mkdir()
        for name in ("auth.json", "settings.json", "models.json", "config.toml", "history.jsonl"):
            (config / name).write_text("{}\n")
        (config / "sessions").mkdir()
        for agent, expected in (("pi", {"auth.json", "models.json", "settings.json"}),):
            work = root / agent
            work.mkdir()
            master, slave = pty.openpty()
            proc = subprocess.Popen([binary, "sandbox", "--image", image, "--agent", agent,
                                     "--config-dir", config, "--", "argument with spaces", "quote'"],
                                    cwd=work, stdin=slave, stdout=slave, stderr=slave)
            try:
                wait_for_prompt(proc, master)
                assert (work / "dispatch.txt").read_text().splitlines() == [agent, "argument with spaces", "quote'"]
                assert set((work / "config-files.txt").read_text().splitlines()) == expected
                assert (config / "auth.json").read_text() == "{}\n"
                os.write(master, b"\x01x")
                assert proc.wait(5) == 0
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
                os.close(master)
                os.close(slave)
        print("Fixture Pi: /work, arguments, exact config allowlists, return to shell verified")


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("bazel-bin/k-vm"))
    parser.add_argument("--image", type=Path, default=Path("images/linux-riscv64"))
    args = parser.parse_args()
    check(args.binary.resolve(), args.image.resolve())
