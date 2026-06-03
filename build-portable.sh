#!/bin/bash
# Build SLSsteam with maximum portability on current system
# For best results, run on the OLDEST distro you want to support

set -e

echo "========================================="
echo "  SLSsteam Portable Build"
echo "========================================="
echo ""
echo "This will compile SLSsteam on your current system."
echo ""
echo "IMPORTANT:"
echo "  - Forward compatible: binary works on NEWER glibc versions"
echo "  - Not backward compatible: binary may NOT work on OLDER glibc"
echo ""

# Check current glibc version
GLIBC_VERSION=$(ldd --version | head -1 | awk '{print $NF}')
echo "Your glibc version: $GLIBC_VERSION"
echo ""
echo "This binary will work on systems with glibc >= $GLIBC_VERSION"
echo ""

read -p "Continue? [y/N] " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Build cancelled."
    exit 0
fi

# Check dependencies
echo ""
echo "Checking dependencies..."

MISSING_DEPS=()

if ! command -v g++ &> /dev/null; then
    MISSING_DEPS+=("g++")
fi

if ! command -v make &> /dev/null; then
    MISSING_DEPS+=("make")
fi

if ! pkg-config --exists openssl 2>/dev/null; then
    MISSING_DEPS+=("libssl-dev or openssl-devel")
fi

if ! pkg-config --exists libcurl 2>/dev/null; then
    MISSING_DEPS+=("libcurl4-openssl-dev or libcurl-devel")
fi

# Check for 32-bit libs specifically
if ! pkg-config --exists openssl --32 2>/dev/null && ! [ -f /usr/lib/i386-linux-gnu/pkgconfig/openssl.pc ]; then
    MISSING_DEPS+=("32-bit openssl (lib32-openssl or libssl-dev:i386)")
fi

if ! pkg-config --exists libcurl --32 2>/dev/null && ! [ -f /usr/lib/i386-linux-gnu/pkgconfig/libcurl.pc ]; then
    MISSING_DEPS+=("32-bit libcurl (lib32-curl or libcurl4-openssl-dev:i386)")
fi

if [ ${#MISSING_DEPS[@]} -gt 0 ]; then
    echo ""
    echo "ERROR: Missing dependencies:"
    for dep in "${MISSING_DEPS[@]}"; do
        echo "  - $dep"
    done
    echo ""
    echo "Install on Ubuntu/Debian:"
    echo "  sudo dpkg --add-architecture i386"
    echo "  sudo apt update"
    echo "  sudo apt install g++-multilib make libssl-dev:i386 libcurl4-openssl-dev:i386"
    echo ""
    echo "Install on Arch:"
    echo "  sudo pacman -S multilib/lib32-gcc-libs multilib/lib32-openssl multilib/lib32-curl"
    echo ""
    exit 1
fi

echo "✓ All dependencies found"
echo ""

# Set PKG_CONFIG_PATH for 32-bit libs
if [ -d /usr/lib/i386-linux-gnu/pkgconfig ]; then
    export PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig
elif [ -d /usr/lib32/pkgconfig ]; then
    export PKG_CONFIG_PATH=/usr/lib32/pkgconfig
fi

echo "Building SLSsteam..."
make clean
make

echo ""
echo "========================================="
echo "  Build Complete!"
echo "========================================="
echo ""
echo "Verifying binary compatibility..."
echo ""

# Check glibc requirements
echo "GLIBC requirements:"
objdump -T ./bin/SLSsteam.so | grep GLIBC | sed 's/.*GLIBC_/GLIBC_/' | sort -u | head -5
echo ""

echo "Linked libraries:"
ldd ./bin/SLSsteam.so | grep -E 'libc\.so|libstdc\+\+'
echo ""

echo "This binary requires glibc >= $GLIBC_VERSION"
echo ""
echo "Install with: ./setup.sh install"
echo ""
