#!/bin/bash
# Build meshtasticd with the Waveshare bridge backend.
#   meshtastic/build.sh [checkout-dir]     (default: meshtastic/firmware)
# Clones meshtastic/firmware at the commit this was tested against, applies wsbridge.patch, copies the
# libpinedio-usb drop-in from bridge/host, and builds the native-wsbridge PlatformIO env.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
DIR=${1:-$HERE/firmware}
COMMIT=${MESHTASTIC_COMMIT:-c39728e3fb9304fc3b1160ebf14801b3b5f0a213}   # 2.8.2.c39728e

if [ ! -d "$DIR/.git" ]; then
    git clone https://github.com/meshtastic/firmware.git "$DIR"
fi
cd "$DIR"
if ! git diff --quiet; then
    echo "$DIR has local changes; refusing to re-apply the patch" >&2
    exit 1
fi
git checkout -q "$COMMIT"
git submodule update --init --depth 1
git apply "$HERE/wsbridge.patch"

W=src/platform/portduino/wsbridge
mkdir -p "$W/include"
cp "$ROOT/bridge/host/libpinedio_wsbridge.c" "$ROOT/bridge/host/api_bind.c" "$W/"
cp "$ROOT/bridge/host/include/libpinedio-usb.h" "$ROOT/bridge/protocol.h" "$W/include/"

platformio run -e native-wsbridge
echo
echo "Built: $DIR/.pio/build/native-wsbridge/meshtasticd"
