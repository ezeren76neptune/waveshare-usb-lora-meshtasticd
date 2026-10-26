#!/bin/bash
# Host-side tests against a fake bridge on a pty (no hardware needed).
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
CF="-std=gnu17 -Wall -Wextra -O2 -Iinclude -I.. -lpthread"
gcc test/test_shim.c libpinedio_wsbridge.c $CF -o "$T/test_shim"
gcc test/test_unplug.c libpinedio_wsbridge.c $CF -o "$T/test_unplug"
python3 test/fake_bridge.py "$T/pty" & sleep 0.5
WSBRIDGE_PORT="$T/pty" "$T/test_shim"
python3 test/fake_bridge.py "$T/pty2" & FP=$!; sleep 0.5
WSBRIDGE_PORT="$T/pty2" "$T/test_unplug" $FP
