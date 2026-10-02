#!/bin/bash
# [2026-10-02] Install, on a Debian/Ubuntu build machine, the system packages that hold every
# shared library the WolfViewer bundle needs but does not carry, so that dpkg-shlibdeps (in
# wolfviewer-linux-packages.sh deb) can name the package for each one.
#
#   wolfviewer-deb-runtime-libs.sh <bundle.tar.xz>
#
# Nothing is listed by hand: the NEEDED entries of every 64-bit ELF file in the bundle (readelf -d),
# less the sonames the bundle carries and those the dynamic linker already finds (ldconfig -p), are
# looked up with apt-file and the owning packages installed. The 32-bit native SLVoice is left out,
# as in wolfviewer-linux-packages.sh. Needs root (apt-get), file, binutils.
set -euo pipefail

TARBALL="$(readlink -f "${1:?bundle tarball}")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
tar -C "$WORK" -xpf "$TARBALL"

declare -A BUNDLED=()
while IFS= read -r s; do BUNDLED["$s"]=1; done < <(find "$WORK" -type f -name '*.so*' -printf '%f\n')

declare -A NEED=()
while IFS= read -r -d '' f; do
    case "$(file -b "$f")" in ELF\ 64-bit*) ;; *) continue ;; esac
    while read -r so; do
        [ -n "$so" ] && [ -z "${BUNDLED[$so]:-}" ] && NEED["$so"]=1
    done < <(readelf -d "$f" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
done < <(find "$WORK" -type f -print0)

KNOWN="$(ldconfig -p)"
MISSING=()
for so in "${!NEED[@]}"; do
    grep -qF " $so (libc6,x86-64)" <<<"$KNOWN" || MISSING+=("$so")
done
echo "system libraries needed: ${#NEED[@]}, not installed: ${#MISSING[@]}"
[ "${#MISSING[@]}" -eq 0 ] && exit 0

apt-get install -y apt-file >/dev/null
apt-file update >/dev/null
declare -A PKGS=(); UNKNOWN=()
for so in "${MISSING[@]}"; do
    owner="$(apt-file search -l -x "^/(usr/)?lib/x86_64-linux-gnu/${so//./\\.}\$" | head -1)"
    if [ -n "$owner" ]; then PKGS["$owner"]=1; echo "  $so <- $owner"; else UNKNOWN+=("$so"); fi
done
[ "${#UNKNOWN[@]}" -eq 0 ] || { echo "no package provides: ${UNKNOWN[*]}" >&2; exit 1; }
apt-get install -y --no-install-recommends "${!PKGS[@]}"
