#!/usr/bin/env bash
# [WOLFCORE 2026-11-27] Fetch the optional proprietary WolfCore runtime library from private
# intelligentwolf/wolfcore for official IntelligentWolf builds (Linux, Windows, macOS).
#
# LGPL-2.1 compliance & public fork compatibility:
#   - When WOLFCORE_READ_TOKEN is NOT set (any fork, PR, or public open-source build), this script
#     logs a notice and exits 0 without setting WOLF_WOLFCORE_DIR. WolfViewer compiles and packages
#     cleanly using its LGPL-2.1 open-source fallback implementations.
#   - When WOLFCORE_READ_TOKEN IS set (official builds on intelligentwolf/wolfviewer), any failure
#     to download or verify the pinned WolfCore archive fails the build loudly so an official
#     release never silently ships without WolfCore.
set -euo pipefail

if [ -z "${WOLFCORE_READ_TOKEN:-}" ]; then
  echo "WOLFCORE_READ_TOKEN is not set: building open-source viewer without optional proprietary WolfCore module."
  exit 0
fi

WOLFCORE_REPO="${WOLFCORE_REPO:-intelligentwolf/wolfcore}"
WOLFCORE_VERSION="${WOLFCORE_VERSION:-v1.0.0}"

case "${RUNNER_OS:-$(uname -s)}" in
  Linux)
    ASSET="wolfcore-linux64.tar.gz"
    LIB="libwolfcore.so"
    ;;
  Windows|MINGW*|MSYS*|CYGWIN*)
    ASSET="wolfcore-windows64.tar.gz"
    LIB="wolfcore.dll"
    ;;
  macOS|Darwin)
    ASSET="wolfcore-macos64.tar.gz"
    LIB="libwolfcore.dylib"
    ;;
  *)
    echo "::error::install-wolfcore.sh: unsupported platform '${RUNNER_OS:-$(uname -s)}'"
    exit 1
    ;;
esac

WORK="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/wolfcore-${WOLFCORE_VERSION}"
rm -rf "$WORK"
mkdir -p "$WORK/dl" "$WORK/out"

GH_TOKEN="$WOLFCORE_READ_TOKEN" gh release download "$WOLFCORE_VERSION" \
  --repo "$WOLFCORE_REPO" \
  --dir "$WORK/dl" \
  --pattern "$ASSET" \
  --pattern "SHA256SUMS.txt"

cd "$WORK/dl"
WANT=$(awk -v a="$ASSET" '$2 == a || $2 == ("*" a) { print $1 }' SHA256SUMS.txt)
if [ -z "$WANT" ]; then
  echo "::error::install-wolfcore.sh: $ASSET not listed in SHA256SUMS.txt"
  exit 1
fi
if command -v sha256sum >/dev/null 2>&1; then
  GOT=$(sha256sum "$ASSET" | awk '{print $1}')
else
  GOT=$(shasum -a 256 "$ASSET" | awk '{print $1}')
fi
if [ "$GOT" != "$WANT" ]; then
  echo "::error::install-wolfcore.sh: $ASSET sha256 is $GOT, expected $WANT"
  exit 1
fi

tar -xzf "$ASSET" -C "$WORK/out"
test -s "$WORK/out/$LIB" || { echo "::error::install-wolfcore.sh: archive missing $LIB"; exit 1; }

if [ "${RUNNER_OS:-$(uname -s)}" = "macOS" ] || [ "$(uname -s)" = "Darwin" ]; then
  ARCHS=$(lipo -archs "$WORK/out/$LIB")
  for a in arm64 x86_64; do
    case " $ARCHS " in
      *" $a "*) ;;
      *) echo "::error::install-wolfcore.sh: $LIB has slices '$ARCHS', missing $a"; exit 1 ;;
    esac
  done
  EXPORTS=$(nm -gU "$WORK/out/$LIB")
  case "$EXPORTS" in
    *" _wolfcore_get_api"*) ;;
    *) echo "::error::install-wolfcore.sh: $LIB exports no _wolfcore_get_api"; exit 1 ;;
  esac
fi

OUT_PATH="$WORK/out"
if command -v cygpath >/dev/null 2>&1; then
  case "${RUNNER_OS:-}" in
    Windows) OUT_PATH=$(cygpath -w "$WORK/out") ;;
  esac
fi

echo "WOLF_WOLFCORE_DIR=$OUT_PATH" >> "${GITHUB_ENV:-/dev/null}"
echo "WolfCore $WOLFCORE_VERSION ($LIB) ready in $OUT_PATH"
