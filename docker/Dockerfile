# SPDX-License-Identifier: MPL-2.0
# Reproducible linux/amd64 builder image for JieLi AC791N (pi32v2) M-VAVE FM-1 firmware.
FROM --platform=linux/amd64 debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    git \
    make \
    python3 \
    xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Install JieLi pi32v2 LLVM/Clang 4.0.1 toolchain into /opt/jieli/toolchain
RUN mkdir -p /opt/jieli && \
    curl -fL --retry 3 https://pkgman.jieliapp.com/s/linux-toolchain | tar -xJ -C /opt/jieli && \
    TC="$(ls -d /opt/jieli/jieli-linux-toolchains-* | sort | tail -1)" && \
    ln -sfn "${TC}" /opt/jieli/toolchain

ENV JIELI_TOOLCHAIN=/opt/jieli/toolchain
WORKDIR /work
