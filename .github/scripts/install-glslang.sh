#!/usr/bin/env bash
# [2026-11-08] Install a pinned glslang release for the shader pre-compile check
# (scripts/shader_check.py, indra/cmake/ShaderCheck.cmake). Runs under bash on the Linux,
# macOS and Windows (Git Bash) runners.
#
# Why a pinned download and not a package: Ubuntu 22.04's glslang-tools is 11.8 (2021),
# Homebrew and Chocolatey move without notice, and a check that fails or passes because the
# runner's compiler changed is noise. One version, one sha256 per archive, from the project's
# own release page: https://github.com/KhronosGroup/glslang/releases/tag/16.6.0
# The digests are the ones GitHub publishes for the release assets (the releases API `digest`
# field) and were checked against a download.
#
# Exports to the following steps: the bin directory on PATH and GLSLANG_VALIDATOR = the
# executable (the CMake module and the script both read it).
set -euo pipefail

GLSLANG_VERSION="16.6.0"
case "${RUNNER_OS:-$(uname -s)}" in
  Linux)
    ASSET="glslang-${GLSLANG_VERSION}-linux-x86_64-release.zip"
    SHA256="a3fc4f083b1793eb53e55fa3577ac9649ffbe0340715e30d116290fb5382393f" ;;
  macOS|Darwin)
    ASSET="glslang-${GLSLANG_VERSION}-macos-universal-release.zip"
    SHA256="1086bc6c7ce8ff188e4e1d1a69078e9e38156a4c2c3f13e4f91b9289a1d90453" ;;
  Windows|MINGW*|MSYS*)
    ASSET="glslang-${GLSLANG_VERSION}-windows-x86_64-release.zip"
    SHA256="82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b" ;;
  *) echo "::error::install-glslang.sh: unknown OS '${RUNNER_OS:-$(uname -s)}'"; exit 1 ;;
esac
URL="https://github.com/KhronosGroup/glslang/releases/download/${GLSLANG_VERSION}/${ASSET}"

DEST="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/glslang-${GLSLANG_VERSION}"
rm -rf "$DEST"
mkdir -p "$DEST"
cd "$DEST"

# GitHub's release downloads do answer 504 now and then (seen 2026-11-08): retry, and never
# accept a short file - a 92-byte error page is not an archive.
curl -sSL --fail --retry 8 --retry-delay 10 --retry-all-errors -o "$ASSET" "$URL"
if command -v sha256sum >/dev/null 2>&1; then
  GOT=$(sha256sum "$ASSET" | cut -d' ' -f1)
else
  GOT=$(shasum -a 256 "$ASSET" | cut -d' ' -f1)
fi
if [ "$GOT" != "$SHA256" ]; then
  echo "::error::install-glslang.sh: $ASSET sha256 is $GOT, expected $SHA256 (size $(wc -c < "$ASSET") bytes)"
  exit 1
fi

# unzip keeps the executable bits and the bin/glslangValidator -> glslang symlink (Linux, macOS);
# on Windows neither matters and 7-Zip or Python unpacks it.
if command -v unzip >/dev/null 2>&1; then
  unzip -q "$ASSET"
elif command -v 7z >/dev/null 2>&1; then
  7z x -y -bso0 -bsp0 "$ASSET"
else
  python -m zipfile -e "$ASSET" .
fi

# The real executable, whatever the archive calls it (the script and CMake accept both names).
BIN=""
for cand in bin/glslangValidator.exe bin/glslang.exe bin/glslangValidator bin/glslang; do
  if [ -f "$cand" ] && [ ! -L "$cand" ] && [ -s "$cand" ] && [ "$(wc -c < "$cand")" -gt 1000 ]; then
    BIN="$DEST/$cand"
    break
  fi
done
if [ -z "$BIN" ]; then
  echo "::error::install-glslang.sh: no glslang executable in $ASSET"; ls -laR "$DEST" | head -40; exit 1
fi
chmod +x "$BIN" 2>/dev/null || true
"$BIN" --version | head -1

BIN_DIR="$DEST/bin"
if [ "${RUNNER_OS:-}" = "Windows" ] && command -v cygpath >/dev/null 2>&1; then
  BIN=$(cygpath -w "$BIN")
  BIN_DIR=$(cygpath -w "$BIN_DIR")
fi
echo "$BIN_DIR" >> "${GITHUB_PATH:-/dev/null}"
echo "GLSLANG_VALIDATOR=$BIN" >> "${GITHUB_ENV:-/dev/null}"
echo "glslang $GLSLANG_VERSION installed: $BIN"
