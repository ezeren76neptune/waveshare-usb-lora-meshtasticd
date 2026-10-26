#!/bin/bash
# Update the bridge firmware over USB through Archie3d's bootloader (no SWD wiring needed).
#   scripts/flash-usb.sh <path/to/wsprog> [port]
# Stops meshtasticd first. Then: unplug the stick, hold its button, plug it in (TXD+RXD both lit).
set -euo pipefail
cd "$(dirname "$0")/.."
WSPROG=${1:?path to Archie3d wsprog}
make -C bridge/fw
systemctl --user stop meshtasticd 2>/dev/null || true
read -rp "Hold the stick's button while plugging it in (both LEDs lit), then press Enter... "
PORT=${2:-$(readlink -f /dev/serial/by-id/usb-1a86_*)}
"$WSPROG" -p "$PORT" bridge/fw/wsbridge.bin
