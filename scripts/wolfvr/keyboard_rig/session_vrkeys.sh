#!/bin/bash
# session_vrkeys.sh TAG TREE : [VR KEYBOARD 2026-11-05] session_vr.sh, but the viewer runs on its OWN display, opened on
# workspace 9 without focus - KEYS=wayland (default): a bare nested Hyprland (wtype keys); KEYS=x11: a rootful Xwayland
# (xkeys/XTEST keys) - so e2e_keyboard.mjs can press REAL keys into it without ever taking the desktop's keyboard.
# session.sh TAG TREE [amd] : isolated test viewer as Shadow Testwolf (lock held for the whole session), camera
# campaign at Paul's spot on The Caribbean, log out (requestQuit), release the lock. Never Paul's profile or install.
TAG=$1; TREE=$2; GPU=$3; H=$HOME/.cache/skyband; LOG=$H/run-$TAG.out
SLURL="${SLURL:-secondlife://The%20Caribbean/2597/4759/88}"
exec 9>$HOME/.config/wolf/shadow_testwolf.lock
echo "waiting for lock $(date -u +%T)" > $LOG
flock 9
echo "lock held $(date -u +%T)" >> $LOG
# [Shadow 11-05] Paul may have started his viewer (VR) while this session waited for the lock: never alongside him
$H/wait_paul.sh >> $LOG
echo "Paul not in a viewer $(date -u +%T)" >> $LOG
pkill -f "$H/login_vr.py" 2>/dev/null; pkill -f "$H/campaign_vr.py" 2>/dev/null
rm -rf $H/prof; mkdir -p $H/prof/user_settings; : > $H/leap.log
python3 -I $H/mkprof_vr.py "$SLURL" $EXTRA_KEYS >> $LOG 2>&1
cp $H/logcontrol-dev.xml $H/prof/user_settings/
KEYS=${KEYS:-wayland}; export KEYS
if [ "$KEYS" = x11 ]; then
  XDISP=${XDISP:-77}
  hyprctl dispatch "hl.dsp.exec_cmd(\"Xwayland :$XDISP -geometry 2560x1600 -nolisten tcp\", { workspace = \"9 silent\", no_initial_focus = true })" >> $LOG
  for i in $(seq 30); do [ -S /tmp/.X11-unix/X$XDISP ] && break; sleep 0.5; done
  echo "X server :$XDISP $( [ -S /tmp/.X11-unix/X$XDISP ] && echo up || echo MISSING) $(date -u +%T)" >> $LOG
  export DISPLAY=:$XDISP; unset WAYLAND_DISPLAY
else
  # a bare Hyprland nested in the desktop's (nested_hypr.lua), on workspace 9 and opened without focus: its new
  # Wayland socket and instance are the ones that appear now
  R=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
  before_s=$(ls $R | grep -E '^wayland-[0-9]+$' | sort); before_h=$(ls $R/hypr 2>/dev/null | sort)
  hyprctl dispatch "hl.dsp.exec_cmd(\"Hyprland -c $H/nested_hypr.lua > $H/nested_hypr-$TAG.log 2>&1\", { workspace = \"9 silent\", no_initial_focus = true })" >> $LOG
  for i in $(seq 40); do
    NS=$(comm -13 <(echo "$before_s") <(ls $R | grep -E '^wayland-[0-9]+$' | sort) | head -1)
    NH=$(comm -13 <(echo "$before_h") <(ls $R/hypr 2>/dev/null | sort) | head -1)
    [ -n "$NS" ] && [ -n "$NH" ] && [ -S $R/$NS ] && break; sleep 0.5; done
  echo "nested Hyprland ${NS:-MISSING} ${NH:-MISSING} $(date -u +%T)" >> $LOG
  export WAYLAND_DISPLAY=$NS NESTED_HYPR=$NH; unset DISPLAY
fi
( for i in $(seq 180); do sleep 1; grep -q "STATE_LOGIN_WAIT" $H/prof/logs/WolfViewer.log 2>/dev/null && break; done; sleep 6; python3 -I $H/login_vr.py >> $LOG 2>&1 ) 9>&- &
( T0=$(date +%s)
  INW=0
  for i in $(seq 300); do
    grep -q "STATE_STARTED" $H/prof/logs/WolfViewer.log 2>/dev/null && { INW=1; break; }
    grep -q "Login failed\|NAME NOT CONFIRMED" $H/prof/logs/WolfViewer.log $LOG 2>/dev/null && break
    sleep 1
  done
  if [ $INW = 1 ]; then echo "in world $(date -u +%T)" >> $LOG
  else echo "LOGIN NOT COMPLETED (failed, name not confirmed, or 300 s) $(date -u +%T): no retry" >> $LOG; fi
  [ $INW = 1 ] && sleep 45
  printf "LLWindow\t{'op':'mouseDown','button':'LEFT','path':'//OK'}\n" > $H/leap.fifo; printf "LLWindow\t{'op':'mouseUp','button':'LEFT','path':'//OK'}\n" > $H/leap.fifo
  [ $INW = 1 ] && python3 -I $H/${CAMPAIGN:-campaign.py} "$KEYS" $TAG >> $LOG 2>&1
  # only while this session's viewer runs, and never blocking: a FIFO write with no reader would wait for the NEXT session's relay and quit that viewer
  WP=$(cat $H/viewer-$TAG.pid 2>/dev/null); [ -n "$WP" ] && kill -0 $WP 2>/dev/null && timeout 5 $H/sendquit.sh
  # only THIS session's viewer (its pid file), 30 s after requestQuit: SIGKILL, so the lock is never held on a hang
  # (the pid file holds the wrapper script's pid; the viewer binary is its child)
  WP=$(cat $H/viewer-$TAG.pid 2>/dev/null)
  for i in $(seq 30); do [ -n "$WP" ] && kill -0 $WP 2>/dev/null || break; sleep 1; done
  if [ -n "$WP" ] && kill -0 $WP 2>/dev/null; then
    for VP in $(pgrep -P $WP); do [[ "$(readlink /proc/$VP/exe)" == "$TREE"/bin/* ]] || continue
      timeout 180 gdb -iex "set sysroot /" "$(readlink /proc/$VP/exe)" -p $VP -batch -ex "thread apply all bt 30" > $H/hang-bt-$TAG.txt 2>&1
      kill -KILL $VP && echo "had to KILL $VP $(date -u +%T) (backtrace hang-bt-$TAG.txt)" >> $LOG; done
  fi ) 9>&- &
[ "$GPU" = amd ] && export __GLX_VENDOR_LIBRARY_NAME=mesa DRI_PRIME=pci-0000_c5_00_0
export TREE H TAG
rm -f $H/viewer-$TAG.pid
unshare -Urm bash -c '
  mount --bind "$H/prof" "$HOME/.wolfviewer_x64" || exit 3
  exec unshare -U --map-user=1000 --map-group=1000 bash -c "
    echo \$\$ > \"\$H/viewer-\$TAG.pid\"; cd \"\$TREE\" && exec ./wolfviewer --skipupdatecheck --grid grid.wolfterritories.org:8002 --leap \"python3 -I \$H/leapbin/relay.py\"
  "
' 9>&- >> $LOG 2>&1
echo "exit $? $(date -u +%T)" >> $LOG
# [RIG 2026-11-05] nothing of this session may outlive it: the viewer gets no lock fd (9>&- above), and anything
# still running from this tree now (a viewer relaunched by its self-updater's detached "wolfviewer-relaunch" waiter,
# etc/wolfviewer-update.sh) is stopped - sessions hold the lock, so no other session can be using $TREE at this moment
for p in /proc/[0-9]*; do pid=${p#/proc/}; [ "$pid" = $$ ] && continue
  e=$(readlink $p/exe 2>/dev/null); c=$( { tr '\0' ' ' < $p/cmdline; } 2>/dev/null)
  # also this session's headless Chromium (e2e scripts start it with --user-data-dir=shots/TAG/profile; a test script
  # stopped half-way leaves it running, connected to the next viewer's headset port as a second headset)
  if [[ "$e" == "$TREE"/bin/* ]] || [[ "$c" == *wolfviewer-relaunch*"$TREE"* ]] || [[ "$c" == *"user-data-dir=$H/shots/$TAG/"* ]]; then
    kill -TERM $pid 2>/dev/null && echo "stopped leftover $pid ($e ${c:0:80}) $(date -u +%T)" >> $LOG; fi; done
sleep 3
for p in /proc/[0-9]*; do e=$(readlink $p/exe 2>/dev/null); [[ "$e" == "$TREE"/bin/* ]] && kill -KILL ${p#/proc/} 2>/dev/null && echo "killed leftover ${p#/proc/} $(date -u +%T)" >> $LOG; done
grep -q "sendLogoutRequest\|Logging out\|logout" $H/prof/logs/WolfViewer.log && echo "logout seen" >> $LOG
cp $H/prof/logs/WolfViewer.log $H/log-$TAG.log 2>/dev/null
if [ "$KEYS" = x11 ]; then pkill -f "^Xwayland :$XDISP " && echo "X server :$XDISP stopped $(date -u +%T)" >> $LOG
else pkill -f "^Hyprland -c $H/nested_hypr.lua" && echo "nested Hyprland stopped $(date -u +%T)" >> $LOG; fi
echo "lock released $(date -u +%T)" >> $LOG
