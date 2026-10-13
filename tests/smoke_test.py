#!/usr/bin/env python3
"""Exercise the actual CLI and HTTP config loader without guest images."""
import http.server
import os
import subprocess
import struct
import threading
import unittest
from pathlib import Path


BINARY = Path(os.environ.get("K_VM_BINARY", "bazel-bin/k-vm")).resolve()


class SmokeTest(unittest.TestCase):
    def setUp(self):
        self.assertTrue(BINARY.is_file(), "k-vm binary has not been built")

    def test_help_uses_project_name(self):
        result = subprocess.run([BINARY, "-h"], capture_output=True, text=True, timeout=10)
        self.assertIn("k-vm version", result.stdout)
        self.assertIn("usage: k-vm", result.stdout)

    def test_http_config_is_downloaded_and_parsed(self):
        requests = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                body = b'{ version: 1, machine: "http-test-unknown-machine", memory_size: 32 }'
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_args):
                pass

        server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            result = subprocess.run(
                [BINARY, f"http://127.0.0.1:{server.server_port}/vm.cfg"],
                capture_output=True, text=True, timeout=15,
                env={**os.environ, "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1"},
            )
            self.assertEqual(requests, ["/vm.cfg"])
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("http-test-unknown-machine", result.stderr)
        finally:
            server.shutdown()
            worker.join()
            server.server_close()

    def test_riscv_boot_with_http_filesystem_and_user_network(self):
        # RV32 instructions also execute on RV64/RV128: write 1 to HTIF tohost, then its upper word to dispatch.
        bios = struct.pack("<5I", 0x400082B7, 0x00100313, 0x0062A023, 0x0002A223, 0x0000006F)
        for xlen in (32, 64, 128):
            with self.subTest(xlen=xlen):
                requests = []

                class Handler(http.server.BaseHTTPRequestHandler):
                    def do_GET(self):
                        path = self.path.split("?", 1)[0]
                        requests.append(path)
                        responses = {
                            "/vm.cfg": (
                                '{ version: 1, machine: "riscv%d", memory_size: 32, '
                                'bios: "bios.bin", fs0: { file: "http://127.0.0.1:%d/fs" }, '
                                'eth0: { driver: "user" } }'
                                % (xlen, self.server.server_port)
                            ).encode(),
                            "/bios.bin": bios,
                            "/fs/head": b"Version: 1\nRevision: 1\nRootID: 1\n",
                            "/fs/files/0000000000000001": b"Version: 1\nRevision: 1\n\n",
                        }
                        body = responses.get(path)
                        self.send_response(200 if body is not None else 404)
                        body = body or b""
                        self.send_header("Content-Length", str(len(body)))
                        self.end_headers()
                        self.wfile.write(body)

                    def log_message(self, *_args):
                        pass

                server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
                worker = threading.Thread(target=server.serve_forever, daemon=True)
                worker.start()
                try:
                    result = subprocess.run(
                        [BINARY, f"http://127.0.0.1:{server.server_port}/vm.cfg"],
                        capture_output=True, text=True, timeout=15,
                        env={**os.environ, "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1"},
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("Power off.", result.stdout)
                    for path in ("/vm.cfg", "/bios.bin", "/fs/head", "/fs/files/0000000000000001"):
                        self.assertIn(path, requests)
                finally:
                    server.shutdown()
                    worker.join()
                    server.server_close()


if __name__ == "__main__":
    unittest.main()
