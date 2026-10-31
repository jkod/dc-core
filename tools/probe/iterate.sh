#!/bin/sh
# Grow a game's function list by running it: every "MISS <addr>" from the boot probe becomes a
# function start, the changed files are regenerated and rebuilt, and the probe runs again.
#   tools/probe/iterate.sh DISC.cue 1ST_READ.BIN WORKDIR [rounds] [seconds]
# WORKDIR/funcs.txt is the result (commit it to the port's repo).
set -e
DISC=$1; BIN=$2; W=$3; ROUNDS=${4:-10}; SECS=${5:-20}
CORE=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$W/gen"
[ -f "$W/funcs.txt" ] || python3 "$CORE/tools/recomp/sh4recomp.py" "$BIN" --out "$W/gen" --write-funcs "$W/funcs.txt" >/dev/null
i=0
while [ $i -lt "$ROUNDS" ]; do
  python3 "$CORE/tools/recomp/sh4recomp.py" "$BIN" --funcs "$W/funcs.txt" --out "$W/gen" --write-funcs "$W/funcs.txt" | tail -1
  for c in "$W"/gen/*.c; do
    o="${c%.c}.o"; [ "$o" -nt "$c" ] && continue
    echo "$c"
  done | xargs -r -P 2 -I{} sh -c 'cc -O1 -w -fno-strict-aliasing -I"$1/runtime/include" -c "$2" -o "${2%.c}.o"' _ "$CORE" {}
  rm -f "$W/librecomp.a"; ar rcs "$W/librecomp.a" "$W"/gen/*.o
  cc -O1 -w -I"$CORE/runtime/include" "$CORE/tools/probe/boot_probe.c" "$CORE"/runtime/src/*.c "$W/librecomp.a" -lm -o "$W/boot_probe"
  OUT=$("$W/boot_probe" "$DISC" "$BIN" "$SECS" 2>&1 || true)
  echo "$OUT" | tail -6 > "$W/last_probe.txt"
  A=$(echo "$OUT" | grep -o 'MISS [0-9A-F]*' | head -1 | cut -d' ' -f2)
  if [ -z "$A" ] || [ "$A" = "00000000" ]; then echo "$OUT" | tail -25; exit 0; fi
  echo "round $i: new function $A"
  echo "$A" | tr 'A-F' 'a-f' >> "$W/funcs.txt"
  i=$((i + 1))
done
cat "$W/last_probe.txt"
