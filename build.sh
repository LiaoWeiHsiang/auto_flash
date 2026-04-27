#!/bin/bash

rm -rf build
set -a
source config.env
set +a

# ===== Windows build =====
if [ "$BUILD_AUTO_FLASH" = "ON" ] || [ "$BUILD_FLASH_EXE" = "ON" ]; then
    rm -rf build-win
    cmake -S . -B build-win \
        -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw64.cmake \
        -DBUILD_AUTO_FLASH=$BUILD_AUTO_FLASH \
        -DBUILD_FLASH_EXE=$BUILD_FLASH_EXE

    cmake --build build-win
fi

# ===== Linux build =====
if [ "$BUILD_HOST_SERVER" = "ON" ]; then
    rm -rf build-linux
    cmake -S . -B build-linux \
        -DBUILD_HOST_SERVER=$BUILD_HOST_SERVER

    cmake --build build-linux

    # ===== copy web files =====
    cp host_server/index.html build-linux/
fi