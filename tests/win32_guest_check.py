#!/usr/bin/env python3
"""Exercise the native EXE with pipes, on Windows or under Wine (no WSL)."""
import argparse
import http.server
import os
import queue
import subprocess
import tempfile
import threading
import time
from pathlib import Path


class Guest:
    def __init__(self, command, cwd):
        self.proc = subprocess.Popen(command, cwd=cwd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.chunks = queue.Queue()
        self.output = bytearray()
        def read():
            while chunk := os.read(self.proc.stdout.fileno(), 4096):
                self.chunks.put(chunk)
            self.chunks.put(None)
        threading.Thread(target=read, daemon=True).start()

    def until(self, marker, timeout=90):
        output = bytearray()
        deadline = time.monotonic() + timeout
        while marker not in output:
            try:
                chunk = self.chunks.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                raise RuntimeError(f"guest timeout: {bytes(self.output)!r}")
            if chunk is None:
                raise RuntimeError(f"guest exited: {bytes(self.output)!r}")
            output.extend(chunk)
            self.output.extend(chunk)
        return bytes(output)

    def send(self, data):
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def command(self, command):
        self.send(command.encode() + b"\n")
        return self.until(b"/work # ")

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--wine", help="Optional Wine executable for cross-build verification")
    parser.add_argument("--pi", action="store_true", help="Also check real Pi, Node and registry HTTPS")
    args = parser.parse_args()
    def host_path(path):
        path = str(Path(path).resolve())
        return "Z:" + path.replace("/", "\\") if args.wine else path
    prefix = [args.wine] if args.wine else []
    with tempfile.TemporaryDirectory(prefix="k-vm-native-") as temporary:
        root = Path(temporary)
        work = root / "project 汉字 with spaces"
        work.mkdir()
        (work / "project.txt").write_bytes(b"project\n")
        config = root / "config"
        config.mkdir()
        (config / "auth.json").write_bytes(b"{}\n")
        (config / "history.jsonl").write_bytes(b"private\n")
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b"NATIVE_NETWORK_OK\n")
            def log_message(self, *_):
                pass
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        command = prefix + [str(args.binary.resolve()), "sandbox", "--image", host_path(args.image),
                            "--agent", "shell", "--config-dir", host_path(config)]
        guest = Guest(command, work)
        try:
            guest.until(b"/work # ")
            assert b"project\r\n" in guest.command("cat project.txt"), bytes(guest.output)
            guest.command("echo guest-write > result.txt")
            assert (work / "result.txt").read_bytes() == b"guest-write\n"
            result = guest.command("find /run/k-vm/config -type f")
            assert b"auth.json" not in result and b"history.jsonl" not in result, result
            guest.command("printf '#!/bin/sh\\necho SCRIPT_OK\\n' > run.sh; chmod +x run.sh")
            assert b"SCRIPT_OK\r\n" in guest.command("./run.sh"), bytes(guest.output)
            result = guest.command(f"/bin/busybox wget -q -O - http://10.0.2.2:{server.server_port}/")
            assert b"NATIVE_NETWORK_OK\r\n" in result, result
            guest.send(b"sleep 60\n")
            time.sleep(0.3)
            guest.send(b"\x03")
            guest.until(b"/work # ", timeout=10)
            assert guest.proc.poll() is None, "Ctrl+C terminated the sandbox"
            if args.pi:
                assert b"v24." in guest.command("node --version")
                result = guest.command("pi --version")
                assert b"not found" not in result, result
                result = guest.command("node -e 'fetch(\"https://registry.npmjs.org/@earendil-works/pi-coding-agent/latest\").then(r=>{if(!r.ok)process.exit(1);console.log(\"HTTPS_OK\")}).catch(()=>process.exit(1))'")
                assert b"HTTPS_OK\r\n" in result, result
            guest.send(b"exit\n")
            assert guest.proc.wait(30) == 0, bytes(guest.output)
            print("Native EXE guest: Unicode /work, writes, executable modes, config exclusion, SLIRP, Ctrl+C and graceful exit passed")
        finally:
            guest.close()
            server.shutdown()


if __name__ == "__main__":
    main()
