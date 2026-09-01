#!/bin/bash

# GoogleTest Linux Static Library Build and Deploy Script
#
# Used only by the optional framework-comparison build (-DENABLE_GTEST_COMPARISON=ON).
# The project's test suite itself is Catch2; see README "Framework comparison".

set -e  # Exit on error

GTEST_VERSION="1.15.2"

echo "========================================"
echo "GoogleTest v${GTEST_VERSION} Static Library Build Script"
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
    echo "After installation, run this script again."
    exit 1
fi
echo "  g++ found: $(g++ --version | head -1)"

TARBALL="googletest-${GTEST_VERSION}.tar.gz"
if [ ! -f "$TARBALL" ]; then
    echo "[ERROR] $TARBALL not found in $SCRIPT_DIR"
    exit 1
fi

# Extract tarball
echo "[1/4] Extracting $TARBALL..."
rm -rf tmp_build
mkdir tmp_build
tar -xzf "$TARBALL" -C tmp_build

SRC_DIR="tmp_build/googletest-${GTEST_VERSION}/googletest"

# Deploy headers
echo "[2/4] Deploying headers to include/..."
mkdir -p include
cp -r "$SRC_DIR/include/gtest" include/

# Compile static libraries (fused build: gtest-all.cc pulls in every source file)
echo "[3/4] Compiling static libraries (C++17)..."
mkdir -p lib/linux
g++ -std=c++17 -O2 -c "$SRC_DIR/src/gtest-all.cc" \
    -I "$SRC_DIR/include" -I "$SRC_DIR" -o tmp_build/gtest-all.o
g++ -std=c++17 -O2 -c "$SRC_DIR/src/gtest_main.cc" \
    -I "$SRC_DIR/include" -o tmp_build/gtest_main.o
ar rcs lib/linux/libgtest.a tmp_build/gtest-all.o
ar rcs lib/linux/libgtest_main.a tmp_build/gtest_main.o

# Cleanup
echo "[4/4] Cleaning up temporary files..."
rm -rf tmp_build

echo ""
echo "========================================"
echo "[SUCCESS] GoogleTest build completed!"
echo "========================================"
echo ""
echo "Output files:"
echo "  Headers:   include/gtest/"
echo "  Libraries: lib/linux/libgtest.a, lib/linux/libgtest_main.a"
echo ""
echo "Usage in your project:"
echo "  1. Include: #include <gtest/gtest.h>  (-Iinclude)"
echo "  2. Link:    g++ your_test.o -Llib/linux -lgtest_main -lgtest -pthread"
echo "  (main() is provided by libgtest_main)"
echo ""
echo "Done!"
