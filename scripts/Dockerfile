# Builder image for slsteam-moon's portable build.
#
# Ubuntu 22.04 -> glibc 2.34. Resulting binary works on any distro
# with glibc >= 2.34: Ubuntu 22.04+, Debian 12+, Fedora 36+, modern
# Mint/Pop!_OS/Zorin, etc.
#
# Used by scripts/build.sh --portable (which invokes
# scripts/_build-portable.sh). Source is bind-mounted at run time;
# this image only carries the toolchain.
FROM ubuntu:22.04

# Install 32-bit toolchain and the dev headers the Makefile links.
# (-m32 builds; SLSsteam.so must match Steam's 32-bit ubuntu12_32 client.)
RUN dpkg --add-architecture i386 && \
    apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        g++-multilib \
        libcurl4-openssl-dev:i386 \
        libssl-dev:i386 \
        make \
        pkg-config && \
    rm -rf /var/lib/apt/lists/*

# 32-bit pkg-config search path (matches scripts/_build-host.sh).
ENV PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig

WORKDIR /build
