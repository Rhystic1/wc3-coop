#!/bin/zsh
# Install the wc3net XanLib proxy into the game directory, reversibly.
# The original XANLIB.DLL is renamed to XanLibReal.dll. The proxy forwards
# every game import back to it.
set -e
GAME="$HOME/Library/Application Support/CrossOver/Bottles/GOG Galaxy/drive_c/GOG Games/Wing Commander III 30th Anniversary Edition/Kilrathi_Saga"
PROXY="$HOME/Projects/wc3-coop/src/wc3net/XanLib.dll"

if [[ ! -f "$GAME/XanLibReal.dll" ]]; then
  mv "$GAME/XANLIB.DLL" "$GAME/XanLibReal.dll"
  echo "original moved to XanLibReal.dll"
fi
cp "$PROXY" "$GAME/XanLib.dll"
echo "proxy installed"
