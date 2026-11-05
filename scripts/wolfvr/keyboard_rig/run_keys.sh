#!/bin/bash
# run_keys.sh TAG [TREE] (KEYS=wayland|x11) : [VR KEYBOARD 2026-11-05] scripts/wolfvr/e2e_keyboard.mjs (real key presses into the viewer's own X
# server) against a Shadow Testwolf session (session_vrkeys.sh: lock, throwaway profile, private X server), port 4361
~/.cache/skyband/wait_paul.sh || exit 1
cd ~/.cache/skyband
grep -q . nullsink.stages 2>/dev/null || pactl load-module module-null-sink sink_name=wolfvr_null > nullsink.stages
export PULSE_SINK=wolfvr_null
WT=${WT:-$HOME/NetBeansProjects/wolfviewer-vrkeys}
TREE=${2:-$HOME/.cache/skyband/tkeys}
cc -O2 -Wall -o $HOME/.cache/skyband/xkeys $WT/scripts/wolfvr/xkeys.c -lX11 -lXtst || exit 1
export XKEYS=$HOME/.cache/skyband/xkeys VIEWER_TREE=$TREE KEYS=${KEYS:-wayland}
# Wayland keys need the viewer as a native Wayland window (WolfNativeWayland, as residents who switched it on - Paul)
WLKEY=$([ "$KEYS" = wayland ] && echo WolfNativeWayland=Boolean:1 || echo WolfNativeWayland=Boolean:0)
EXTRA_KEYS="$WLKEY WolfVRDebugStats=Boolean:1 WolfVR3DMode=Boolean:1 WolfVRStreamMode=String:world WolfVRStreamPort=U32:4361 $MORE_KEYS" \
 VR_SCRIPT=$WT/scripts/wolfvr/e2e_keyboard.mjs VR_ARGS=$WT/scripts/wolfvr/fake_xr_controllers.js \
 VR_TOGGLE=0 VR_URL=http://127.0.0.1:4361/ CAMPAIGN=campaign_vr.py ./session_vrkeys.sh "$1" $TREE
