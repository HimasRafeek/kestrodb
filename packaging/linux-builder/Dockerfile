# Build environment for the Linux release binaries (x64 native, arm64 cross-compiled).
# Ubuntu 22.04 has glibc 2.35, so the binaries need glibc 2.34 or newer (not the build host's
# 2.39): Ubuntu 22.04+, Debian 12+, RHEL/Rocky/Alma 9+, Fedora 35+, Amazon Linux 2023.
#   docker build -t kestrodb-linux-builder packaging/linux-builder
#   docker run --rm -u $(id -u):$(id -g) -v "$PWD":/src -w /src kestrodb-linux-builder bash scripts/build-linux.sh
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
# arm64 packages (libssl-dev:arm64) come from ports.ubuntu.com; the main archive is amd64 only.
RUN sed -i 's/^deb http/deb [arch=amd64] http/' /etc/apt/sources.list \
 && printf '%s\n' \
    'deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy main universe' \
    'deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-updates main universe' \
    'deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-security main universe' \
    > /etc/apt/sources.list.d/arm64.list \
 && dpkg --add-architecture arm64 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
    gcc-12 g++-12 gcc-12-aarch64-linux-gnu g++-12-aarch64-linux-gnu \
    cmake ninja-build make file binutils binutils-aarch64-linux-gnu \
    libssl-dev libssl-dev:arm64 qemu-user-static ca-certificates \
 && rm -rf /var/lib/apt/lists/*
# The arm64 OpenSSL headers on their own (see cmake/aarch64-linux-gnu.cmake).
RUN mkdir -p /opt/arm64-openssl/include \
 && cp -r /usr/include/openssl /opt/arm64-openssl/include/ \
 && cp /usr/include/aarch64-linux-gnu/openssl/* /opt/arm64-openssl/include/openssl/ \
 && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 100 \
 && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 100 \
 && update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-12 100 \
 && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-12 100
