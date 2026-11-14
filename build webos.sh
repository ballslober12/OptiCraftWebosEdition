#!/bin/sh
# Build and package the webOS edition.
#
#   ./build\ webos.sh
#   WEBOS_SDK=/path/to/arm-webos-linux-gnueabi_sdk-buildroot ./build\ webos.sh
#
# Run it from this directory (or any directory on a Linux/WSL host with the
# webOS SDK toolchain unpacked). The IPK lands in dist/.
set -e

SDK="${WEBOS_SDK:-/root/webos/arm-webos-linux-gnueabi_sdk-buildroot}"
BUILD_DIR="${BUILD_DIR:-build/webos}"
TOOLCHAIN="$SDK/share/buildroot/toolchainfile.cmake"

if [ ! -f "$TOOLCHAIN" ]; then
    echo "webOS toolchain not found: $TOOLCHAIN" >&2
    echo "Set WEBOS_SDK to the arm-webos-linux-gnueabi_sdk-buildroot directory." >&2
    exit 1
fi

cmake -B "$BUILD_DIR" -DPLATFORM=WEBOS -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
cmake --build "$BUILD_DIR" --parallel
cmake --build "$BUILD_DIR" --target webos-package

echo
echo "IPK written to dist/:"
ls -1 dist/*.ipk 2>/dev/null || echo "  (none found)"
