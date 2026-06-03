# Building SLSsteam

SLSsteam is a 32-bit shared library that hooks into Steam using `LD_AUDIT`. Building it correctly ensures maximum compatibility across different Linux distributions.

## Understanding GLIBC Compatibility

**Key Concept:** Binaries are **forward compatible** with glibc.
- Binary compiled on **glibc 2.17** → works on 2.17, 2.31, 2.39, 2.43, etc. ✅
- Binary compiled on **glibc 2.43** → does NOT work on 2.39, 2.31, etc. ❌

The upstream AceSLS/SLSsteam releases work everywhere because they're compiled in an old environment (likely glibc 2.17 or older).

## Build Methods

### Method 1: Docker Build (Recommended for Distribution)

Produces a binary compatible with **glibc 2.17+** (CentOS 7, Ubuntu 14.04+, Debian 8+, all modern distros).

```bash
# Install Docker first if needed
sudo apt install docker.io          # Ubuntu/Debian
sudo dnf install docker              # Fedora
sudo pacman -S docker                # Arch

sudo systemctl enable --now docker
sudo usermod -aG docker $USER
newgrp docker  # or logout/login

# Build
./build-docker.sh
```

This creates binaries in `./bin/` that work on virtually any modern Linux distro.

**Pros:**
- ✅ Maximum compatibility
- ✅ No pollution of host system
- ✅ Reproducible builds
- ✅ Same method used by upstream

**Cons:**
- ❌ Requires Docker installed
- ❌ Slower (first build downloads container image)

### Method 2: Local Build (Quick Development)

Compiles using your system's toolchain. **Only works on systems with glibc >= your version.**

```bash
# Install dependencies
## Ubuntu/Debian
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install g++-multilib make libssl-dev:i386 libcurl4-openssl-dev:i386

## Arch Linux
sudo pacman -S multilib/lib32-gcc-libs multilib/lib32-openssl multilib/lib32-curl

## Fedora
sudo dnf install gcc-c++ make openssl-devel.i686 libcurl-devel.i686

# Build
make clean && make
```

**Pros:**
- ✅ Fast iteration for development
- ✅ No Docker needed

**Cons:**
- ❌ Binary only works on glibc >= your version
- ❌ Not portable for distribution

Use `./build-portable.sh` for interactive guided build with dependency checking.

### Method 3: Old Distro VM (Alternative to Docker)

If you can't or don't want to use Docker, compile on an old distro in a VM:

1. Create VM with **Ubuntu 20.04 LTS** or **Debian 11** (glibc 2.31)
2. Install dependencies
3. Compile SLSsteam
4. Copy `./bin/*` back to host

**Pros:**
- ✅ Good compatibility (glibc 2.31+)
- ✅ No Docker needed

**Cons:**
- ❌ Manual VM management
- ❌ Not as old as Docker method (2.31 vs 2.17)

## Verifying Binary Compatibility

After building, check what glibc version is required:

```bash
# Check highest GLIBC version required
objdump -T ./bin/SLSsteam.so | grep GLIBC_ | sed 's/.*GLIBC_//' | sort -V | tail -1

# Check linked libraries
ldd ./bin/SLSsteam.so

# Your system's glibc version
ldd --version
```

## Build Flags Explained

From `Makefile`:

```makefile
CXXFLAGS := -O3 -flto=auto -fPIC -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0
```

- `-m32`: Compile 32-bit (Steam's ubuntu12_32 runtime is 32-bit)
- `-D_GLIBCXX_USE_CXX11_ABI=0`: Use old C++ ABI for compatibility
- `-O3 -flto=auto`: Optimize for size and performance
- `-fPIC`: Position-independent code (required for shared libraries)

These flags are **critical** for compatibility and are identical to upstream.

## Troubleshooting

### "wrong ELF class: ELFCLASS32"
The Steam process you're hooking is 64-bit. SLSsteam only works with 32-bit Steam processes.

Steam Linux uses both:
- `steam` (64-bit launcher)
- `ubuntu12_32/steam` (32-bit client - this is what we hook)

Solution: Make sure you're injecting into the 32-bit process. The `setup.sh` wrapper handles this correctly.

### "GLIBC_X.XX not found"
Binary was compiled on a newer system. Rebuild locally or use Docker build.

### "cannot open shared object file: No such file or directory"
Missing 32-bit runtime libraries:

```bash
# Ubuntu/Debian
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install libssl3:i386 libcurl4:i386

# Arch
sudo pacman -S lib32-openssl lib32-curl
```

### Docker build fails with "permission denied"
Add yourself to docker group:

```bash
sudo usermod -aG docker $USER
newgrp docker
```

## For Distribution Maintainers

**If you're packaging SLSsteam for a distro:**

1. **Use Docker build** to ensure compatibility across users' systems
2. **OR compile on the oldest supported distro version** in your repo
3. Include both `SLSsteam.so` and `library-inject.so` in the package
4. Install to `/usr/share/SLSsteam/` or `/opt/SLSsteam/`
5. Let users run `setup.sh install` to configure their environment

## CI/CD Recommendations

For GitHub Actions or GitLab CI:

```yaml
- name: Build SLSsteam (manylinux)
  run: |
    docker build -f Dockerfile.build -t slssteam-builder .
    docker create --name builder slssteam-builder
    docker cp builder:/build/bin ./bin
    docker rm builder
```

This ensures release binaries work on all user systems.
