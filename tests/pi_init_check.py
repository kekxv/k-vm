#!/usr/bin/env python3
"""Real guest checks for initialization persistence, automatic launch and rollback."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import pty
import select
import shutil
import subprocess
import tempfile
import time
from agent_workspace_check import wait_for_prompt


def run_install(binary, image, *options, timeout=30, log=None, cancel_on=None):
    master, slave = pty.openpty()
    proc = subprocess.Popen([binary, 'init', '--image', image, '--agent', 'pi', *options],
                            cwd='/', stdin=slave, stdout=slave, stderr=slave)
    output = b''
    deadline = time.monotonic() + timeout
    try:
        while proc.poll() is None:
            if time.monotonic() > deadline:
                proc.terminate()
                raise RuntimeError(f'init timeout: {output[-5000:]!r}')
            if select.select([master], [], [], .1)[0]:
                data = os.read(master, 65536)
                output += data
                if cancel_on and cancel_on in output:
                    proc.terminate()
                    cancel_on = None
                if log:
                    with open(log, 'ab') as file:
                        file.write(data)
        while select.select([master], [], [], 0)[0]:
            output += os.read(master, 65536)
        return proc.returncode, output
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait()
        os.close(master); os.close(slave)


def inject(disk, source, destination):
    subprocess.run(['debugfs', '-w', '-R', f'write {source} {destination}', disk],
                   check=True, capture_output=True)


def check(binary, source):
    with tempfile.TemporaryDirectory(prefix='k-vm-pi-init-') as temporary:
        root = Path(temporary)
        image = root / 'image'
        shutil.copytree(source, image)
        disk = image / 'root-riscv64.bin'
        (image / 'sandbox.json').write_text(json.dumps({'agents': ['shell'], 'pi_bootstrap': 1}))
        script = root / 'installer'
        script.write_text('''#!/bin/sh
set -eu
if grep -q ' work ' /proc/mounts; then exit 90; fi
test ! -e /root/.pi/agent/auth.json
echo partial > /root/install-state
if [ "$2" = fail ]; then exit 42; fi
if [ "$2" = slow ]; then echo INIT_SLEEPING; sleep 30; fi
if [ -f /root/pi-version ]; then exit 0; fi
echo 1.2.3 > /root/pi-version
printf '#!/bin/sh\ncat /root/pi-version\n' > /usr/bin/pi
chmod +x /usr/bin/pi
''')
        script.chmod(0o755)
        for path in ('/usr/local', '/usr/local/sbin'):
            subprocess.run(['debugfs', '-w', '-R', f'mkdir {path}', disk], capture_output=True, check=True)
        inject(disk, script, '/usr/local/sbin/k-vm-install-pi')
        before = hashlib.sha256(disk.read_bytes()).digest()
        status, output = run_install(binary, image, '--version', 'fail')
        assert status != 0, output
        assert hashlib.sha256(disk.read_bytes()).digest() == before
        assert not Path(str(disk) + '.pi-ready').exists()
        status, output = run_install(binary, image, '--version', 'slow', cancel_on=b'INIT_SLEEPING')
        assert status != 0, output
        assert hashlib.sha256(disk.read_bytes()).digest() == before
        assert not Path(str(disk) + '.pi-ready').exists()

        # First Pi launch installs before mapping credentials or the project.
        work = root / 'project with spaces'
        config = root / 'credentials'
        work.mkdir(); config.mkdir()
        (config / 'auth.json').write_text('{"fixture":"secret"}')
        for first in (True, False):
            master, slave = pty.openpty()
            proc = subprocess.Popen([binary, 'sandbox', '--image', image, '--agent', 'pi',
                                     '--config-dir', config, '--', '--version'],
                                    cwd=work, stdin=slave, stdout=slave, stderr=slave)
            try:
                output = wait_for_prompt(proc, master, 30)
                assert b'1.2.3' in output and b'agent ended (0)' in output, output
                assert (b'initializing Node.js' in output) == first, output
                os.write(master, b'echo persisted > result.txt\n')
                wait_for_prompt(proc, master)
                os.write(master, b'exit\n')
                assert proc.wait(5) == 0
            finally:
                if proc.poll() is None: proc.kill(); proc.wait()
                os.close(master); os.close(slave)
        assert (work / 'result.txt').read_text() == 'persisted\n'
        assert Path(str(disk) + '.pi-ready').exists()
        assert not list(image.glob('*.update-*'))
        status, output = run_install(binary, image)
        assert status == 0, output
        # Changing the disk invalidates the readiness hint and must prevent offline autoinit.
        changed = root / 'changed'
        changed.write_text('disk changed\n')
        inject(disk, changed, '/root/changed')
        result = subprocess.run([binary, 'sandbox', '--image', image, '--agent', 'pi', '--network', 'none'],
                                capture_output=True, text=True, timeout=5)
        assert result.returncode != 0 and 'k-vm init' in result.stderr, result.stderr
        print('Guest init: rollback/cancellation, isolated automatic installation, persistent Pi, readiness invalidation')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('bazel-bin/k-vm'))
    parser.add_argument('--image', type=Path, default=Path('images/linux-riscv64'))
    args = parser.parse_args()
    check(args.binary.resolve(), args.image.resolve())
