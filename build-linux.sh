#!/bin/sh
# Builds build-linux/ofp: the lifted C on pcrecomp's runtime/win32hle.
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build-linux}
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i386.cmake $CMAKE_ARGS
fi
cmake --build "$BUILD_DIR" "$@"
