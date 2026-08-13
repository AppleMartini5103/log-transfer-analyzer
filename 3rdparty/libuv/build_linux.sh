#!/bin/bash

# libuv Linux Shared Library Build and Deploy Script

set -e  # Exit on error

echo "========================================"
echo "libuv 1.51.0 Shared Library Build Script"
echo "(Ubuntu/Linux Build)"
echo "========================================"
echo ""

# Save current script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Check dependencies
echo "[0/5] Checking dependencies..."

# Check CMake
if ! command -v cmake &> /dev/null; then
    echo ""
    echo "[ERROR] CMake is not installed!"
    echo ""
    echo "Please install CMake with one of these commands:"
    echo "  Ubuntu/Debian: sudo apt-get install cmake"
    echo "  Fedora:        sudo dnf install cmake"
    echo "  Arch:          sudo pacman -S cmake"
    echo ""
    echo "After installation, run this script again."
    echo ""
    exit 1
fi

echo "[SUCCESS] CMake found: $(cmake --version | head -n1)"

# Check build tools
if ! command -v make &> /dev/null; then
    echo ""
    echo "[ERROR] Make is not installed!"
    echo ""
    echo "Please install build tools:"
    echo "  Ubuntu/Debian: sudo apt-get install build-essential"
    echo "  Fedora:        sudo dnf groupinstall 'Development Tools'"
    echo "  Arch:          sudo pacman -S base-devel"
    echo ""
    exit 1
fi

echo "[SUCCESS] Make found"

# Check GCC
if ! command -v gcc &> /dev/null; then
    echo ""
    echo "[ERROR] GCC is not installed!"
    echo ""
    echo "Please install GCC:"
    echo "  Ubuntu/Debian: sudo apt-get install build-essential"
    echo "  Fedora:        sudo dnf install gcc"
    echo "  Arch:          sudo pacman -S gcc"
    echo ""
    exit 1
fi

echo "[SUCCESS] GCC found: $(gcc --version | head -n1)"
echo ""

# Path settings - MODIFIED for lib/window/async structure
TAR_GZ_FILE="libuv-v1.51.0.tar.gz"
EXTRACT_DIR="libuv-v1.51.0"
LIB_OUTPUT_DIR="lib/linux/async"
INCLUDE_OUTPUT_DIR="include"

# 1. Check tar.gz file
if [ ! -f "$TAR_GZ_FILE" ]; then
    echo "[ERROR] $TAR_GZ_FILE not found!"
    echo "Please download libuv source from https://github.com/libuv/libuv/releases"
    exit 1
fi

echo "[1/5] Extracting $TAR_GZ_FILE..."

# Remove old extracted files
if [ -d "$EXTRACT_DIR" ]; then
    echo "Removing old extracted files..."
    rm -rf "$EXTRACT_DIR"
fi

# Extract tar.gz file
tar -xzf "$TAR_GZ_FILE"

if [ ! -d "$EXTRACT_DIR" ]; then
    echo "[ERROR] Failed to extract $TAR_GZ_FILE"
    exit 1
fi

echo "[SUCCESS] Extraction completed"
echo ""

# 2. Create output directories - MODIFIED to create lib/window/async
echo "[2/5] Creating output directories..."
mkdir -p "$LIB_OUTPUT_DIR"
mkdir -p "$INCLUDE_OUTPUT_DIR"
echo "Created: $LIB_OUTPUT_DIR"
echo "Created: $INCLUDE_OUTPUT_DIR"
echo ""

# 3. Configure CMake for shared library build
echo "[3/5] Configuring libuv build with CMake..."
cd "$EXTRACT_DIR"

mkdir -p build
cd build

# Configure with CMake
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DLIBUV_BUILD_TESTS=OFF \
    -DLIBUV_BUILD_BENCH=OFF

if [ $? -ne 0 ]; then
    echo ""
    echo "[ERROR] CMake configuration failed!"
    echo ""
    echo "Please make sure you have all required development packages installed."
    echo ""
    cd ../..
    exit 1
fi

echo "[SUCCESS] CMake configuration completed"
echo ""

# 4. Build
echo "[4/5] Building libuv shared library..."

# Get number of CPU cores for parallel build
NPROC=$(nproc 2>/dev/null || echo 4)
echo "Building with $NPROC parallel jobs..."

make -j$NPROC

if [ $? -ne 0 ]; then
    echo ""
    echo "[ERROR] Build failed!"
    echo ""
    cd ../..
    exit 1
fi

echo "[SUCCESS] Build completed"
echo ""

# 5. Copy files
echo "[5/5] Copying files to output directories..."

# Copy shared library files
if [ -f "libuv.so.1.0.0" ]; then
    cp -v "libuv.so.1.0.0" "../../$LIB_OUTPUT_DIR/"
    echo "Copied libuv.so.1.0.0 to $LIB_OUTPUT_DIR"
fi

if [ -f "libuv.so.1" ]; then
    cp -v "libuv.so.1" "../../$LIB_OUTPUT_DIR/"
    echo "Copied libuv.so.1 to $LIB_OUTPUT_DIR"
fi

if [ -f "libuv.so" ]; then
    cp -v "libuv.so" "../../$LIB_OUTPUT_DIR/"
    echo "Copied libuv.so to $LIB_OUTPUT_DIR"
fi

# Create symlinks if main .so file exists
cd "../../$LIB_OUTPUT_DIR"
if [ -f "libuv.so.1.0.0" ]; then
    ln -sf libuv.so.1.0.0 libuv.so.1 2>/dev/null || true
    ln -sf libuv.so.1 libuv.so 2>/dev/null || true
    echo "Created symlinks: libuv.so.1 -> libuv.so.1.0.0"
    echo "                  libuv.so -> libuv.so.1"
fi
cd "$SCRIPT_DIR"

cd "$EXTRACT_DIR/build"

# Copy pkg-config file if exists
if [ -f "libuv.pc" ]; then
    cp -v "libuv.pc" "../../$LIB_OUTPUT_DIR/"
    echo "Copied libuv.pc to $LIB_OUTPUT_DIR"
fi

# Copy static library if exists (optional)
if [ -f "libuv_a.a" ]; then
    cp -v "libuv_a.a" "../../$LIB_OUTPUT_DIR/"
    echo "Copied libuv_a.a to $LIB_OUTPUT_DIR"
fi

cd ../..

# Copy header files
echo ""
echo "Copying header files..."
mkdir -p "$INCLUDE_OUTPUT_DIR/uv"
cp -v "$EXTRACT_DIR/include/uv.h" "$INCLUDE_OUTPUT_DIR/"
cp -v "$EXTRACT_DIR/include/uv/"*.h "$INCLUDE_OUTPUT_DIR/uv/"

echo ""
echo "========================================"
echo "[SUCCESS] libuv build completed!"
echo "========================================"
echo ""
echo "Output files:"
echo "  Library: $LIB_OUTPUT_DIR/libuv.so.1.0.0"
echo "           $LIB_OUTPUT_DIR/libuv.so.1 (symlink)"
echo "           $LIB_OUTPUT_DIR/libuv.so (symlink)"
echo "  Headers: $INCLUDE_OUTPUT_DIR/uv.h"
echo "           $INCLUDE_OUTPUT_DIR/uv/*.h"
echo ""
echo "Usage in your project:"
echo "  1. Compile: gcc -c your_code.c -I$INCLUDE_OUTPUT_DIR"
echo "  2. Link: gcc your_code.o -L$LIB_OUTPUT_DIR -luv -lpthread -ldl"
echo "  3. Include: #include <uv.h>"
echo ""
echo "Runtime library path:"
echo "  Add to LD_LIBRARY_PATH: export LD_LIBRARY_PATH=\$PWD/$LIB_OUTPUT_DIR:\$LD_LIBRARY_PATH"
echo "  Or install to system: sudo cp $LIB_OUTPUT_DIR/libuv.so* /usr/local/lib/"
echo "                        sudo ldconfig"
echo ""
echo "Cleaning up temporary files..."
rm -rf "$EXTRACT_DIR"
echo "Done!"
echo ""