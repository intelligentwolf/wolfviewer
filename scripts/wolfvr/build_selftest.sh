#!/bin/bash
# [VR PIPE 2026-11-03] Builds indra/newview/tests/wolfvrpipe_selftest against the SAME packaged Boost, OpenSSL and
# libjpeg-turbo the viewer links (build-linux-x86_64/packages), so the tests exercise the code as the viewer ships it.
#   scripts/wolfvr/build_selftest.sh [out_dir]    -> <out_dir>/wolfvrpipe_selftest
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PKG="${WOLFVR_PACKAGES:-$ROOT/build-linux-x86_64/packages}"
OUT="${1:-$ROOT/build-linux-x86_64/wolfvr-selftest}"
mkdir -p "$OUT"
NV="$ROOT/indra/newview"
c++ -std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -DBOOST_BIND_GLOBAL_PLACEHOLDERS \
    -isystem "$PKG/include" \
    "$NV/wolfvrpipe.cpp" "$NV/wolfvrpairing.cpp" "$NV/wolfvrpipe_video.cpp" "$NV/tests/wolfvrpipe_selftest.cpp" "$NV/qrcodegen/qrcodegen.cpp" \
    -o "$OUT/wolfvrpipe_selftest" \
    "$PKG/lib/release/libboost_json.a" "$PKG/lib/release/libboost_container.a" \
    "$PKG/lib/release/libssl.a" "$PKG/lib/release/libcrypto.a" "$PKG/lib/release/libjpeg.a" "$PKG/lib/release/libz.a" \
    -ldl -lpthread
echo "$OUT/wolfvrpipe_selftest"
