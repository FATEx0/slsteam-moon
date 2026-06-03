#!/bin/bash
# Build SLSsteam using Docker/Podman for maximum compatibility
# This produces a binary compatible with glibc 2.17+ (works on all major distros from 2014+)

set -e

# Resolve script dir and switch to repo root so relative paths
# (Dockerfile, ./bin, ./setup.sh) work regardless of where this is invoked.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

echo "========================================="
echo "  SLSsteam Container Build"
echo "========================================="
echo ""
echo "This will build SLSsteam in an Ubuntu 22.04 container"
echo "for broad compatibility (glibc 2.34+)"
echo ""

# Detect container runtime (Podman or Docker)
CONTAINER_CMD=""
if command -v podman &> /dev/null; then
    CONTAINER_CMD="podman"
    echo "✓ Using Podman"
elif command -v docker &> /dev/null; then
    CONTAINER_CMD="docker"
    echo "✓ Using Docker"
else
    echo "ERROR: Neither Podman nor Docker is installed!"
    echo ""
    echo "Install one of them:"
    echo ""
    echo "Podman (rootless, recommended):"
    echo "  Ubuntu/Debian: sudo apt install podman"
    echo "  Fedora: sudo dnf install podman"
    echo "  Arch: sudo pacman -S podman"
    echo ""
    echo "Docker:"
    echo "  Ubuntu/Debian: sudo apt install docker.io"
    echo "  Fedora: sudo dnf install docker"
    echo "  Arch: sudo pacman -S docker"
    echo "  Then: sudo systemctl enable --now docker"
    echo "        sudo usermod -aG docker \$USER"
    echo ""
    exit 1
fi

# Check if user needs permissions (Docker only, Podman is rootless)
if [ "$CONTAINER_CMD" = "docker" ]; then
    if ! groups | grep -q docker && [ "$EUID" -ne 0 ]; then
        echo "WARNING: You may need to run Docker with sudo"
        echo "To fix: sudo usermod -aG docker $USER && newgrp docker"
        echo ""
    fi
fi

# Build container image
echo ""
echo "Building container image..."
$CONTAINER_CMD build -f scripts/build/Dockerfile.build -t slssteam-builder .

# Extract compiled binaries
echo ""
echo "Extracting binaries..."
CONTAINER_ID=$($CONTAINER_CMD create slssteam-builder)
$CONTAINER_CMD cp "$CONTAINER_ID:/build/bin/." ./bin/
$CONTAINER_CMD rm "$CONTAINER_ID"

echo ""
echo "========================================="
echo "  Build Complete!"
echo "========================================="
echo ""
echo "Binaries are in ./bin/"
echo ""
echo "Verify compatibility:"
echo "  ldd ./bin/SLSsteam.so"
echo "  objdump -p ./bin/SLSsteam.so | grep GLIBC"
echo ""
echo "Install:"
echo "  ./setup.sh install"
echo ""
