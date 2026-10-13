#!/bin/sh
# PID1 bootstrap installed in the image; launch data is a readonly 9P export.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
mkdir -p /proc /sys /dev /run /work
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mkdir -p /run/k-vm
if ! mount -t 9p -o trans=virtio,version=9p2000.L,msize=8192,ro kv_launch /run/k-vm; then
    echo 'k-vm: missing launch export; check sandbox arguments' >&2
    exec /bin/sh
fi
# A controlling terminal lets Ctrl+C reach the guest foreground process group.
if setsid --help 2>&1 | grep -q -- --ctty; then
    setsid --ctty /bin/sh /run/k-vm/entry.sh </dev/hvc0 >/dev/hvc0 2>&1
else
    setsid cttyhack /bin/sh /run/k-vm/entry.sh </dev/hvc0 >/dev/hvc0 2>&1
fi
sync
/bin/busybox poweroff -f
while :; do sleep 3600; done
