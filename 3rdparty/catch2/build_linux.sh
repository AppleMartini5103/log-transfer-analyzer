#!/bin/bash

# Catch2 Linux Static Library Build and Deploy Script

set -e  # Exit on error

CATCH2_VERSION="3.15.3"

echo "========================================"
echo "Catch2 v${CATCH2_VERSION} Static Library Build Script"
echo "(Ubuntu/Linux Build)"
echo "========================================"
echo ""

# Save current script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Check dependencies
echo "[0/4] Checking dependencies..."

if ! command -v g++ &> /dev/null; then
    echo ""
    echo "[ERROR] g++ is not installed!"
    echo ""
    echo "Please install g++ with one of these commands:"
    echo "  Ubuntu/Debian: sudo apt-get install g++"
    echo "  Fedora:        sudo dnf install gcc-c++"
    echo "  Arch:          sudo pacman -S gcc"
    echo ""
    echo "Or let the project installer work out the package names for you:"
    echo "  ../../install_deps_linux.sh"
    echo ""
    echo "After installation, run this script again."
    exit 1
fi
echo "  g++ found: $(g++ --version | head -1)"

TARBALL="Catch2-v${CATCH2_VERSION}.tar.gz"
if [ ! -f "$TARBALL" ]; then
    echo "[ERROR] $TARBALL not found in $SCRIPT_DIR"
    exit 1
fi

# Extract tarball
echo "[1/4] Extracting $TARBALL..."
rm -rf tmp_build
mkdir tmp_build
tar -xzf "$TARBALL" -C tmp_build

SRC_DIR="tmp_build/Catch2-${CATCH2_VERSION}/extras"

# Deploy header
echo "[2/4] Deploying amalgamated header to include/..."
mkdir -p include
cp "$SRC_DIR/catch_amalgamated.hpp" include/

# Compile static library
echo "[3/4] Compiling static library (C++17)..."
mkdir -p lib/linux
g++ -std=c++17 -O2 -c "$SRC_DIR/catch_amalgamated.cpp" \
    -I include -o tmp_build/catch_amalgamated.o
ar rcs lib/linux/libcatch2.a tmp_build/catch_amalgamated.o

# Cleanup
echo "[4/4] Cleaning up temporary files..."
rm -rf tmp_build

echo ""
echo "========================================"
echo "[SUCCESS] Catch2 build completed!"
echo "========================================"
echo ""
echo "Output files:"
echo "  Header:  include/catch_amalgamated.hpp"
echo "  Library: lib/linux/libcatch2.a"
echo ""
echo "Usage in your project:"
echo "  1. Include: #include <catch_amalgamated.hpp>  (-Iinclude)"
echo "  2. Link:    g++ your_test.o -Llib/linux -lcatch2"
echo "  (default main() is provided by the library)"
echo ""
echo "Done!"
