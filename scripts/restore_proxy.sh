#!/bin/zsh
# Restore the original XANLIB.DLL and remove the wc3net proxy.
set -e
GAME="$HOME/Library/Application Support/CrossOver/Bottles/GOG Galaxy/drive_c/GOG Games/Wing Commander III 30th Anniversary Edition/Kilrathi_Saga"
rm -f "$GAME/XanLib.dll"
if [[ -f "$GAME/XanLibReal.dll" ]]; then
  mv "$GAME/XanLibReal.dll" "$GAME/XANLIB.DLL"
  echo "original restored"
fi
echo "proxy removed"
