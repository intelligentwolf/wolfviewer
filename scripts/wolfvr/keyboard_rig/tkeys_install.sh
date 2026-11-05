#!/bin/bash
# tkeys_install.sh [binary] : the VR keyboard agent's OWN test tree ~/.cache/skyband/tkeys (never ~/firestorm, never tvr):
# stripped binary into both slots, libllwebrtc.so, and the worktree's app_settings + skins (no delete).
set -euo pipefail
S=~/NetBeansProjects/wolfviewer-vrkeys; B=$S/build-linux-x86_64; T=~/.cache/skyband/tkeys
[ -d $T ] || cp -a --reflink=always ~/.cache/skyband/tvr $T
objcopy --strip-debug ${1:-$B/newview/firestorm-bin} $T/bin/do-not-directly-run-wolfviewer-bin
cp $T/bin/do-not-directly-run-wolfviewer-bin $T/bin/do-not-directly-run-wolfviewer-bin-avx2
objcopy --strip-debug $B/llwebrtc/libllwebrtc.so $T/lib/libllwebrtc.so
rsync -a $S/indra/newview/app_settings/ $T/app_settings/
rsync -a $S/indra/newview/skins/ $T/skins/
ls -la $T/bin/do-not-directly-run-wolfviewer-bin-avx2
