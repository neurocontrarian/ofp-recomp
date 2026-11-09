#!/bin/sh
# Builds build-linux64/ofp: the lifted C on pcrecomp's runtime/win32hle, as a
# 64-bit program (the guest gets a 4 GB block of its own).
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build-linux64}
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  cmake -S . -B "$BUILD_DIR" -G Ninja $CMAKE_ARGS
fi
cmake --build "$BUILD_DIR" "$@"
