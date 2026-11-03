#!/bin/bash
# [VR BIG SCREEN 2026-11-03] install_local.sh STRIPPED_BIN : this branch (W93 "Leah's Lanterns" + VR in your headset)
# into ~/firestorm, the avx2 slot the launcher runs on Paul's CPU. Modelled on ~/wolfviewer-releases/w93-work/install_w93.sh.
# Every file it replaces is kept in ~/viewer-backups/vrbigscreen-before-install-<UTC> first (rsync --backup-dir), and
# the backup folder gets restore.sh. Refuses while a viewer runs from ~/firestorm.
set -euo pipefail
BIN=$1
DEST=$HOME/firestorm
NV=/home/paulc/NetBeansProjects/wolfviewer-vrpipe/indra/newview
for p in /proc/[0-9]*; do
  exe=$(readlink "$p/exe" 2>/dev/null || true)
  [[ "$exe" == "$DEST"/* ]] && { echo "REFUSED: ${p#/proc/} runs $exe"; exit 2; }
done
BK=$HOME/viewer-backups/vrbigscreen-before-install-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$BK/bin"
cp -a "$DEST/wolfviewer" "$BK/"
cp -a "$DEST/bin/do-not-directly-run-wolfviewer-bin-avx2" "$BK/bin/"
rsync -ai --backup --backup-dir="$BK/app_settings" "$NV/app_settings/" "$DEST/app_settings/" > "$BK/rsync-app_settings.txt"
rsync -ai --backup --backup-dir="$BK/skins" "$NV/skins/" "$DEST/skins/" > "$BK/rsync-skins.txt"   # ">f+++++++++" = new files
# Restore: the old binary and launcher back, every replaced file back, and the files this install added removed.
cat > "$BK/restore.sh" <<EOF
#!/bin/bash
set -euo pipefail
for p in /proc/[0-9]*; do e=\$(readlink "\$p/exe" 2>/dev/null || true); [[ "\$e" == "$DEST"/* ]] && { echo "close WolfViewer first"; exit 2; }; done
cp -a "$BK/bin/do-not-directly-run-wolfviewer-bin-avx2" "$DEST/bin/"
cp -a "$BK/wolfviewer" "$DEST/"
for d in app_settings skins; do
  grep '^>f+++++++++' "$BK/rsync-\$d.txt" | cut -c13- | while IFS= read -r f; do rm -f "$DEST/\$d/\$f"; done
  [ -d "$BK/\$d" ] && cp -a "$BK/\$d/." "$DEST/\$d/"
done
echo "restored $DEST from $BK"
EOF
chmod +x "$BK/restore.sh"
cp --reflink=auto -f "$BIN" "$DEST/bin/do-not-directly-run-wolfviewer-bin-avx2"
echo "installed $BIN into $DEST; backup $BK; restore: $BK/restore.sh"
