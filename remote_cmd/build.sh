#!/bin/bash
set -e

cd "$(dirname "$0")"

rm -rf build-linux build-win

# ===== Linux (native) =====
cmake -S . -B build-linux
cmake --build build-linux

# ===== Windows (MinGW cross-compile) =====
cmake -S . -B build-win \
    -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw64.cmake
cmake --build build-win

echo ""
echo "Build outputs:"
echo "  build-linux/remote_cmd_host"
echo "  build-linux/remote_cmd_agent"
echo "  build-win/remote_cmd_host.exe"
echo "  build-win/remote_cmd_agent.exe"
