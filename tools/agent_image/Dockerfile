FROM --platform=linux/riscv64 debian:trixie-slim AS base
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    xz-utils busybox-static util-linux net-tools iproute2 ca-certificates git ripgrep curl python3 \
    && rm -rf /var/lib/apt/lists/*
COPY k-vm-init.sh /sbin/k-vm-init
COPY k-vm-install-pi.sh /usr/local/sbin/k-vm-install-pi
RUN chmod 0755 /sbin/k-vm-init /usr/local/sbin/k-vm-install-pi \
    && ln -sf /bin/busybox /sbin/cttyhack && mkdir -p /work /run/k-vm

FROM base AS pi
# Node's community RV64 binaries; prepare_agent_image.py checks the publisher SHA256.
COPY node-riscv64.tar.xz /tmp/node.tar.xz
RUN tar -xJf /tmp/node.tar.xz -C /usr/local --strip-components=1 \
    && rm /tmp/node.tar.xz
ARG PI_VERSION=1.2.0
ENV npm_config_cache=/tmp/npm-cache
RUN /usr/local/sbin/k-vm-install-pi --version ${PI_VERSION} \
    && rm -rf /tmp/npm-cache /root/.npm
