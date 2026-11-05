#!/bin/bash
# [W94 VR CRASH 2026-11-05] The VR pipe built the way the Linux RELEASE is built (g++ 11, Ubuntu 22.04: the wolf-gcc11
# Docker image) with AddressSanitizer, its test server started, then coro_churn.py against it. W94 shipped a g++ 11-only
# heap corruption at headset disconnect that the gcc 16 local builds never showed; run this before every release.
#   scripts/wolfvr/coro_check.sh [seconds=60] [compiler image=wolf-gcc11]   -> exit 0 = clean
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SECS=${1:-60}; IMG=${2:-wolf-gcc11}
OUT=$(mktemp -d "${TMPDIR:-/tmp}/coro_check.XXXXXX")
NV=$ROOT/indra/newview; PKG=$ROOT/build-linux-x86_64/packages
cat > "$OUT/b.sh" <<EOS
set -e
BZ=()
for f in blocksort bzlib compress crctable decompress huffman randtable; do gcc-11 -O2 -g -w -c $NV/bzip2/\$f.c -o $OUT/bz2_\$f.o; BZ+=($OUT/bz2_\$f.o); done
g++-11 -std=c++20 -O2 -g -fsanitize=address -fno-omit-frame-pointer -w -DBOOST_BIND_GLOBAL_PLACEHOLDERS -isystem $PKG/include \
  $NV/wolfvrpipe.cpp $NV/wolfvrdevice.cpp $NV/wolfvrpipe_video.cpp $NV/tests/wolfvrpipe_selftest.cpp $NV/qrcodegen/qrcodegen.cpp \
  $NV/wolfvr_openh264.cpp $NV/wolfvrcodecfetch.cpp \${BZ[@]} -o $OUT/selftest \
  $PKG/lib/release/libboost_json.a $PKG/lib/release/libboost_container.a $PKG/lib/release/libssl.a $PKG/lib/release/libcrypto.a \
  $PKG/lib/release/libjpeg.a $PKG/lib/release/libz.a -ldl -lpthread
EOS
echo "building with $IMG (a few minutes)..."
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:$ROOT:ro" -v "$OUT:$OUT" "$IMG" nice bash "$OUT/b.sh" > "$OUT/build.log" 2>&1 \
  || { echo "BUILD FAILED: $OUT/build.log"; exit 2; }
PORT=$((4500 + RANDOM % 400)); NAME=coro-check-$PORT
docker run -d --rm --name "$NAME" --network host -u "$(id -u):$(id -g)" -e WOLFVR_PORT=$PORT -e WOLFVR_STATE_DIR="$OUT" \
  -e ASAN_OPTIONS=detect_leaks=0 -e HOME="$OUT" -v "$ROOT:$ROOT:ro" -v "$OUT:$OUT" "$IMG" \
  bash -c "timeout $((SECS + 60)) $OUT/selftest serve $NV/app_settings/wolfvr $((SECS + 50)) jpeg > $OUT/serve.log 2>&1" >/dev/null
for i in $(seq 30); do grep -q "listening on https://" "$OUT/serve.log" 2>/dev/null && break; sleep 1; done
IP=$(grep -o "listening on https://[0-9.]*" "$OUT/serve.log" | head -1 | sed 's#.*https://##')
[ -n "$IP" ] || { echo "SERVER DID NOT START: $OUT/serve.log"; docker stop "$NAME" >/dev/null 2>&1; exit 2; }
python3 -I "$ROOT/scripts/wolfvr/coro_churn.py" "$IP" "$PORT" "$SECS"
sleep 2; docker stop "$NAME" >/dev/null 2>&1
CONN=$(grep -a -c "HEADSET CONNECTED" "$OUT/serve.log"); BAD=$(grep -a -c "ERROR: AddressSanitizer" "$OUT/serve.log")
echo "headsets connected: $CONN, AddressSanitizer errors: $BAD (log $OUT/serve.log)"
[ "$CONN" -gt 0 ] || { echo "NO HEADSET GOT IN: the test did not exercise the WebSocket path"; exit 2; }
[ "$BAD" -eq 0 ] || { grep -a -A25 "ERROR: AddressSanitizer" "$OUT/serve.log" | head -40; exit 1; }
echo CLEAN
