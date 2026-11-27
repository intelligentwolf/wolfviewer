#!/usr/bin/env bash
# [W114-VULKAN MAC 2026-11-26] Fetch a pinned MoltenVK release for the macOS app bundle. macOS runner only.
#
# MoltenVK is the Vulkan driver of the Mac build: viewer_manifest.py (Darwin construct) copies libMoltenVK.dylib into
# WolfViewer.app/Contents/Frameworks, writes Contents/Resources/vulkan/icd.d/MoltenVK_icd.json beside it and its licence
# into Contents/Resources, and wolfvkloader.cpp opens it directly
# (@executable_path/../Frameworks/libMoltenVK.dylib). Licence: Apache-2.0 (MoltenVK/LICENSE in the archive).
#
# Why a pinned download (as install-glslang.sh): one version, one sha256, from the project's own release page
# https://github.com/KhronosGroup/MoltenVK/releases/tag/v1.4.2 (published 2026-07-24). The digest is the one GitHub
# publishes for the asset (releases API `digest`), checked against a download on 2026-11-26. The archive's
# MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib is a universal binary (arm64 + x86_64, the viewer's
# CMAKE_OSX_ARCHITECTURES, indra/cmake/Variables.cmake), LC_BUILD_VERSION minos 12.0.
#
# Exports to the following steps: WOLF_MOLTENVK_DIR = a folder holding libMoltenVK.dylib, MoltenVK_icd.json (MoltenVK's
# own manifest, read by viewer_manifest.py for its api_version) and LICENSE.
set -euo pipefail

case "${RUNNER_OS:-$(uname -s)}" in
  macOS|Darwin) ;;
  *) echo "::error::install-moltenvk.sh is for the macOS runner only"; exit 1 ;;
esac

MOLTENVK_VERSION="v1.4.2"
ASSET="MoltenVK-macos.tar"
SHA256="f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e"
URL="https://github.com/KhronosGroup/MoltenVK/releases/download/${MOLTENVK_VERSION}/${ASSET}"

WORK="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/moltenvk-${MOLTENVK_VERSION}"
rm -rf "$WORK"
mkdir -p "$WORK/dl" "$WORK/out"
cd "$WORK/dl"

# GitHub's release downloads do answer 504 now and then: retry, and never accept a short file.
curl -sSL --fail --retry 8 --retry-delay 10 --retry-all-errors -o "$ASSET" "$URL"
GOT=$(shasum -a 256 "$ASSET" | cut -d' ' -f1)
if [ "$GOT" != "$SHA256" ]; then
  echo "::error::install-moltenvk.sh: $ASSET sha256 is $GOT, expected $SHA256 (size $(wc -c < "$ASSET") bytes)"
  exit 1
fi

# Only the three files the bundle needs (paths as listed by tar -tf of this exact archive).
SRC="MoltenVK/MoltenVK/dynamic/dylib/macOS"
tar -xf "$ASSET" "$SRC/libMoltenVK.dylib" "$SRC/MoltenVK_icd.json" "MoltenVK/LICENSE"
cp "$SRC/libMoltenVK.dylib" "$SRC/MoltenVK_icd.json" "$WORK/out/"
cp "MoltenVK/LICENSE" "$WORK/out/LICENSE"

# Both slices the universal viewer runs on, or the Mac it does not cover would find no driver.
ARCHS=$(lipo -archs "$WORK/out/libMoltenVK.dylib")
for a in arm64 x86_64; do
  case " $ARCHS " in
    *" $a "*) ;;
    *) echo "::error::install-moltenvk.sh: libMoltenVK.dylib has slices '$ARCHS', no $a"; exit 1 ;;
  esac
done
# The entry point wolfvkloader.cpp fetches with dlsym (nm's whole output first: grep -q closing the pipe early would make
# nm die of SIGPIPE, which pipefail reports as a failure).
EXPORTS=$(nm -gU "$WORK/out/libMoltenVK.dylib")
case "$EXPORTS" in
  *" _vkGetInstanceProcAddr"*) ;;
  *) echo "::error::install-moltenvk.sh: libMoltenVK.dylib exports no vkGetInstanceProcAddr"; exit 1 ;;
esac

echo "WOLF_MOLTENVK_DIR=$WORK/out" >> "${GITHUB_ENV:-/dev/null}"
echo "MoltenVK $MOLTENVK_VERSION ready in $WORK/out ($ARCHS)"
