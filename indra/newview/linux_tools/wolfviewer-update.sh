#!/bin/bash
# WolfViewer self-update for a tarball install (indra/newview/wolfupdate.cpp installCoro).
#
#   wolfviewer-update.sh apply <tarball> <install dir> <viewer pid>
#
# The viewer has already checked the tarball against the SHA-256 in the signed latest.json. This
# unpacks it BESIDE the install, checks it is a WolfViewer tree, swaps the two folders, keeps the
# old one as <install>-YYYYMMDD_HHMMSS (the backup name install.sh uses, so its pruning sees them)
# and starts the new copy once the running viewer has exited. Nothing is changed until the new
# tree is complete: a failure before the swap leaves the install exactly as it was. Exit 0 only
# when the new copy is in place.
set -euo pipefail

RETAIN_BACKUPS=2

die() { echo "wolfviewer-update: $*" >&2; exit 1; }

[ "${1:-}" = "apply" ] || die "usage: $0 apply <tarball> <install dir> <viewer pid>"
TARBALL="${2:?tarball}"
INSTALL="${3:?install dir}"
VIEWER_PID="${4:?viewer pid}"

[ -f "$TARBALL" ] || die "no tarball at $TARBALL"
[ -d "$INSTALL" ] || die "no install at $INSTALL"
INSTALL="$(cd "$INSTALL" && pwd -P)"
PARENT="$(dirname "$INSTALL")"
BASE="$(basename "$INSTALL")"

# Unpack on the same filesystem as the install, so the swap is two renames.
WORK="$(mktemp -d "$PARENT/.$BASE.update.XXXXXX")" || die "cannot create a work folder in $PARENT"
cleanup() { rm -rf -- "$WORK"; }
trap cleanup EXIT

tar -C "$WORK" -xpf "$TARBALL" || die "could not unpack $TARBALL"

# The tarball holds exactly one top-level folder (viewer_manifest.py LinuxManifest.package_finish).
shopt -s nullglob dotglob
TOPS=("$WORK"/*)
shopt -u nullglob dotglob
[ "${#TOPS[@]}" -eq 1 ] && [ -d "${TOPS[0]}" ] || die "unexpected tarball layout"
NEW="${TOPS[0]}"
[ -f "$NEW/wolfviewer" ] && [ -x "$NEW/bin/do-not-directly-run-wolfviewer-bin" ] \
    || die "the tarball is not a WolfViewer install"

# Top-level files the user added to the old install (a launcher of their own, FS_No_LD_Hacks.txt)
# come along; nothing the new version ships is overwritten, and folders are never copied.
for f in "$INSTALL"/* "$INSTALL"/.[!.]*; do
    [ -e "$f" ] || continue
    name="$(basename "$f")"
    [ -d "$f" ] && continue
    [ -e "$NEW/$name" ] && continue
    cp -a -- "$f" "$NEW/$name"
done

BACKUP="${INSTALL}-$(date +%Y%m%d_%H%M%S)"
mv -- "$INSTALL" "$BACKUP" || die "could not move the old install aside"
if ! mv -- "$NEW" "$INSTALL"; then
    mv -- "$BACKUP" "$INSTALL" || true
    die "could not put the new install in place; the old one is back"
fi

# Keep the newest RETAIN_BACKUPS backups (install.sh prune_old_backups, same name pattern).
shopt -s nullglob
backups=("$INSTALL"-[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]_[0-9][0-9][0-9][0-9][0-9][0-9])
shopt -u nullglob
if [ "${#backups[@]}" -gt "$RETAIN_BACKUPS" ]; then
    mapfile -t backups < <(printf '%s\n' "${backups[@]}" | sort -r)
    for (( i=RETAIN_BACKUPS; i<${#backups[@]}; i++ )); do
        rm -rf -- "${backups[i]}"
    done
fi

# Start the new copy once the viewer that asked for this has gone. Detached, so it outlives us.
setsid nohup bash -c 'while kill -0 "$1" 2>/dev/null; do sleep 0.5; done; exec "$2/wolfviewer"' \
    wolfviewer-relaunch "$VIEWER_PID" "$INSTALL" </dev/null >/dev/null 2>&1 &

exit 0
