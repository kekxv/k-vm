#!/usr/bin/env python3
"""Exercise actual guest update transaction with fixture npm/Pi and sparse root disks."""
import hashlib
import json
import os
import pty
import shutil
import subprocess
import tempfile
from pathlib import Path
from agent_workspace_check import wait_for_prompt


def check(binary, source):
    with tempfile.TemporaryDirectory(prefix="k-vm-pi-update-") as temporary:
        root=Path(temporary)
        image=root/'image'
        shutil.copytree(source,image)
        disk=image/'root-riscv64.bin'
        (image/'sandbox.json').write_text(json.dumps({'agents':['shell','pi']}))
        scripts={
            'npm': '''#!/bin/sh
set -eu
test "$1" = install && test "$2" = -g
test ! -e /root/.pi/agent/auth.json
if grep -q ' work ' /proc/mounts; then exit 90; fi
case "$3" in
  *@fail) echo partial > /root/pi-version; exit 42;;
  *@slow) echo partial > /root/pi-version; echo UPDATE_SLEEPING; sleep 30;;
  *) printf '%s\\n' "$3" > /root/pi-version;;
esac
''',
            'pi': '''#!/bin/sh
cat /root/pi-version
'''}
        for name,text in scripts.items():
            script=root/name;script.write_text(text);script.chmod(0o755)
            subprocess.run(['debugfs','-w','-R',f'write {script} /usr/bin/{name}',disk],
                           check=True,capture_output=True)
        master,slave=pty.openpty()
        try:
            proc=subprocess.Popen([binary,'update','--image',image,'--version','1.2.3'],
                                  cwd='/',stdin=slave,stdout=slave,stderr=slave)
            output=b''
            import select,time
            deadline=time.monotonic()+20
            while proc.poll() is None:
                if time.monotonic()>deadline:
                    proc.kill();raise RuntimeError(output)
                if select.select([master],[],[],0.1)[0]:output+=os.read(master,65536)
            assert proc.returncode==0,output
            saved=subprocess.run(['debugfs','-R','cat /root/pi-version',disk],
                                  check=True,capture_output=True).stdout
            assert saved==b'@earendil-works/pi-coding-agent@1.2.3\n',saved
            before=hashlib.sha256(disk.read_bytes()).digest()
            proc=subprocess.Popen([binary,'update','--image',image,'--version','fail'],
                                  stdin=slave,stdout=slave,stderr=slave)
            deadline=time.monotonic()+20;output=b''
            while proc.poll() is None:
                if time.monotonic()>deadline:
                    proc.kill();raise RuntimeError(output)
                if select.select([master],[],[],0.1)[0]:output+=os.read(master,65536)
            assert proc.returncode!=0,output
            assert hashlib.sha256(disk.read_bytes()).digest()==before
            assert not list(image.glob('*.update-*'))
            proc=subprocess.Popen([binary,'update','--image',image,'--version','slow'],
                                  stdin=slave,stdout=slave,stderr=slave)
            deadline=time.monotonic()+20;output=b''
            while b'UPDATE_SLEEPING' not in output:
                if time.monotonic()>deadline:
                    proc.kill();raise RuntimeError(output)
                if select.select([master],[],[],0.1)[0]:output+=os.read(master,65536)
            proc.terminate()
            assert proc.wait(5)!=0
            assert hashlib.sha256(disk.read_bytes()).digest()==before
            assert not list(image.glob('*.update-*'))
            print('Guest Pi fixture update: persisted version, no work/auth export, failure and interruption preserve original')
        finally:
            if proc.poll() is None:proc.kill();proc.wait()
            os.close(master);os.close(slave)


if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,default=Path('bazel-bin/k-vm'))
    p.add_argument('--image',type=Path,default=Path('images/linux-riscv64'))
    args=p.parse_args();check(args.binary.resolve(),args.image.resolve())
