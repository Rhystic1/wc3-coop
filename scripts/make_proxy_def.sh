#!/bin/zsh
# Generate the XanLib forwarding .def from the game's actual import table.
# Usage: make_proxy_def.sh <WC3W.EXE path> <output .def path>
set -e
EXE="$1"
DEF="$2"
{
  echo "LIBRARY XanLib"
  echo "EXPORTS"
  objdump -p "$EXE" | awk '
    /DLL Name: XanLib.dll/ {inlib=1; next}
    /DLL Name:|^[[:space:]]*lookup/ {inlib=0}
    inlib && /^[[:space:]]+[0-9]+[[:space:]]+\?/ { printf "  %s=XanLibReal.%s\n", $2, $2 }
  ' | sort -u
} > "$DEF"
echo "wrote $DEF"
grep -c "=XanLibReal\." "$DEF"
