FROM --platform=linux/riscv64 debian:trixie-slim
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    xz-utils busybox-static util-linux net-tools iproute2 ca-certificates git ripgrep curl python3 \
    && rm -rf /var/lib/apt/lists/*
# Node's community RV64 binaries; prepare_agent_image.py checks the publisher SHA256.
COPY node-riscv64.tar.xz /tmp/node.tar.xz
RUN tar -xJf /tmp/node.tar.xz -C /usr/local --strip-components=1 \
    && rm /tmp/node.tar.xz
ARG PI_VERSION=1.2.0
RUN npm install -g @earendil-works/pi-coding-agent@${PI_VERSION} \
    && npm cache clean --force \
    && node --version && pi --version \
    && ln -sf /bin/busybox /sbin/cttyhack
COPY k-vm-init.sh /sbin/k-vm-init
RUN chmod 0755 /sbin/k-vm-init && mkdir -p /work /run/k-vm
