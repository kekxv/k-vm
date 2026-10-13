#!/usr/bin/env python3
"""Real guest check of cwd auto-mount and configuration allowlist."""
import argparse
import os
import pty
import subprocess
import tempfile
from pathlib import Path

# The automatic shell starts in /work instead of the legacy home directory.
import time
import select

def wait_for_prompt(proc, master, timeout=15):
    output = b""
    deadline = time.monotonic() + timeout
    while b"/work # " not in output:
        if proc.poll() is not None:
            raise RuntimeError(f"guest exited: {output!r}")
        if time.monotonic() > deadline:
            raise RuntimeError(f"guest timeout: {output!r}")
        ready, _, _ = select.select([master], [], [], 0.2)
        if ready: output += os.read(master, 65536)
    return output


def check(binary, image, agent="shell"):
    with tempfile.TemporaryDirectory(prefix="k-vm-auto-check-") as temporary:
        root = Path(temporary)
        work = root / "project with spaces"
        work.mkdir()
        (work / "project.txt").write_text("project\n")
        config = root / "agent-config"
        config.mkdir()
        (config / "auth.json").write_text('{"test":"credentials"}\n')
        (config / "config.toml").write_text('model = "test"\n')
        (config / "settings.json").write_text('{}\n')
        (config / "models.json").write_text('{}\n')
        (config / "history.jsonl").write_text("private history\n")
        (config / "sessions").mkdir()
        master, slave = pty.openpty()
        proc = None
        try:
            args = [binary, "sandbox", "--image", image, "--agent", agent]
            # Shell must not inherit any agent configuration, even when supplied.
            args += ["--config-dir", config]
            if agent != "shell":
                args += ["--", "--version"]
            proc = subprocess.Popen(args, cwd=work, stdin=slave, stdout=slave, stderr=slave)
            output = wait_for_prompt(proc, master)
            if agent != "shell":
                raise RuntimeError("this check's interactive commands require --agent shell")
            commands = ["pwd", "cat project.txt", "echo guest-write > result.txt",
                        "find /run/k-vm/config -type f", "ls /root/.codex /root/.pi 2>/dev/null || true"]
            output = b""
            for command in commands:
                os.write(master, (command + "\n").encode())
                output += wait_for_prompt(proc, master)
            assert b"/work" in output
            assert (work / "result.txt").read_text() == "guest-write\n"
            assert b"auth.json" not in output
            assert b"history.jsonl" not in output
            os.write(master, b"date +%s > guest-clock.txt\n")
            wait_for_prompt(proc, master)
            assert abs(int((work / "guest-clock.txt").read_text()) - time.time()) < 60
            os.write(master, b"sleep 60\n")
            time.sleep(0.1)
            os.write(master, b"\x03")
            wait_for_prompt(proc, master, timeout=5)
            assert proc.poll() is None, "Ctrl+C ended the sandbox"
            os.write(master, b"\x01x")
            assert proc.wait(5) == 0
            print("Automatic shell: cwd at /work, project writes work, no agent config/history exported, Ctrl+C preserves sandbox")
        finally:
            if proc is not None and proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
            os.close(slave)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("bazel-bin/k-vm"))
    parser.add_argument("--image", type=Path, default=Path("images/linux-riscv64"))
    args = parser.parse_args()
    check(args.binary.resolve(), args.image.resolve())
