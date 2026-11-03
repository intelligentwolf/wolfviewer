#!/bin/bash
# [VR PIPE 2026-11-03] Stage-1 headset test WITHOUT the viewer: the same server, test pattern and encoder WolfViewer
# uses, on this PC's home-network address. Prints the address to type in the Quest Browser in big letters (no code),
# asks "A headset wants to connect" here in the terminal (type a for Allow, d for Deny, then Enter), then a short
# line of numbers every 10 s while a headset is connected. Ctrl+C stops it. Nothing is installed anywhere; the
# certificate and the allowed headsets are kept in ~/.config/wolf/wolfvr-test, so both questions come once only.
#   scripts/wolfvr/run_test_server.sh [minutes=60] [encoder=auto|nvenc|va|x264|openh264|jpeg]
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build-linux-x86_64/wolfvr-selftest/wolfvrpipe_selftest"
NV="$ROOT/indra/newview"
# Rebuild when any of its sources is newer than the binary (or there is none yet).
if [ ! -x "$BIN" ] || [ -n "$(find "$NV"/wolfvrpipe*.cpp "$NV"/wolfvrpipe*.h "$NV"/wolfvrpairing.* "$NV"/tests/wolfvrpipe_selftest.cpp -newer "$BIN" 2>/dev/null)" ]; then
    echo "Building the test server (a few minutes the first time)..."
    nice "$ROOT/scripts/wolfvr/build_selftest.sh" >/dev/null || exit 1
fi
MIN="${1:-60}"
exec "$BIN" serve "$NV/app_settings/wolfvr" $((MIN * 60)) "${2:-auto}"
