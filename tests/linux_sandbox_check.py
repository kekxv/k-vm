#!/usr/bin/env python3
"""Optional end-to-end Linux check; requires a prepared local Linux image."""
import argparse
import hashlib
import http.server
import os
import pty
import select
import signal
import subprocess
import tempfile
import threading
import time
from pathlib import Path


class NetworkHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"NETWORK_OK\n"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def wait_for_prompt(proc, master, timeout=15):
    output = b""
    deadline = time.monotonic() + timeout
    while b"~ # " not in output:
        if proc.poll() is not None:
            raise RuntimeError(f"guest exited {proc.returncode}: {output!r}")
        if time.monotonic() > deadline:
            raise RuntimeError(f"guest prompt timeout: {output!r}")
        ready, _, _ = select.select([master], [], [], 0.2)
        if ready:
            output += os.read(master, 65536)
    return output


def verify(binary, image):
    disk = image / "root-riscv64.bin"
    initial_hash = hashlib.sha256(disk.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="k-vm-linux-check-") as temporary:
        host = Path(temporary)
        (host / "input.txt").write_text("host input\n")
        server = http.server.HTTPServer(("127.0.0.1", 0), NetworkHandler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        master, slave = pty.openpty()
        proc = None
        try:
            started = time.monotonic()
            proc = subprocess.Popen(
                [binary, "sandbox", "--no-workspace", "--image", image,
                 "--map", f"work={host}:rw", "--map", f"input={host / 'input.txt'}"],
                stdin=slave, stdout=slave, stderr=slave,
            )
            wait_for_prompt(proc, master)
            print(f"Full local Linux cold boot to shell: {time.monotonic() - started:.3f}s")
            commands = [
                "mkdir -p /mnt/work /mnt/input",
                "mount -t 9p -o trans=virtio,version=9p2000.L work /mnt/work",
                "mount -t 9p -o trans=virtio,version=9p2000.L input /mnt/input",
                "cat /mnt/input/input.txt",
                "echo guest-output > /mnt/work/output.txt",
                "echo should-fail > /mnt/input/input.txt",
                "echo disposable > /root/k-vm-disposable",
                "ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up",
                "route add default gw 10.0.2.2",
                f"wget -O /tmp/net http://10.0.2.2:{server.server_port}/hello",
                "cat /tmp/net",
            ]
            # Interrupt a real guest foreground command, then continue the shell.
            os.write(master, b"sleep 30\n")
            time.sleep(0.1)
            os.write(master, b"\x03")
            wait_for_prompt(proc, master, timeout=3)
            assert proc.poll() is None, "guest Ctrl+C stopped the sandbox"
            output = b""
            for command in commands:
                os.write(master, (command + "\n").encode())
                output += wait_for_prompt(proc, master)
            assert (host / "output.txt").read_text() == "guest-output\n", "rw mapping did not write to host"
            assert (host / "input.txt").read_text() == "host input\n", "ro mapping altered host file"
            assert b"Operation not permitted" in output, "ro mapping did not reject guest write"
            assert b"NETWORK_OK" in output, "SLIRP HTTP request failed"
            os.write(master, b"\x01x")
            assert proc.wait(5) == 0, "Ctrl+A X did not exit cleanly"
            assert hashlib.sha256(disk.read_bytes()).hexdigest() == initial_hash, "base disk changed"
            print("Linux verified: rw directory, ro single file, HTTP via SLIRP, base disk unchanged, Ctrl+A X clean exit")
        finally:
            if proc is not None and proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
            os.close(slave)
            server.shutdown()
            worker.join()
            server.server_close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("bazel-bin/k-vm"))
    parser.add_argument("--image", type=Path, default=Path("images/linux-riscv64"))
    args = parser.parse_args()
    verify(args.binary.resolve(), args.image.resolve())


if __name__ == "__main__":
    main()
