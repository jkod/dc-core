#!/bin/sh
# Build and run every dc-core test. The OpenGL comparison runs when EGL (Mesa) is installed.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d)
cc -O1 -w -I"$ROOT/runtime/include" "$ROOT/tests/test_runtime.c" "$ROOT"/runtime/src/*.c -lm -o "$T/runtime" && "$T/runtime"
cc -O1 -w -I"$ROOT/runtime/include" "$ROOT/tests/test_pvr_sw.c" "$ROOT"/runtime/src/*.c -lm -o "$T/pvr" && "$T/pvr"
if cc -O1 -w -DWITH_GL -I"$ROOT/runtime/include" "$ROOT/tests/test_pvr_sw.c" "$ROOT"/runtime/src/*.c -lm -lEGL -o "$T/pvrgl" 2>/dev/null; then "$T/pvrgl"; else echo "(no EGL: OpenGL comparison skipped)"; fi
if command -v sdl2-config >/dev/null; then
  cc -O1 -w -I"$ROOT/runtime/include" -I"$ROOT/frontend" "$ROOT/tests/test_front.c" "$ROOT/frontend/dccore_front.c" "$ROOT"/runtime/src/*.c $(sdl2-config --cflags --libs) -lm -o "$T/front"
  SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy "$T/front"
else echo "(no SDL2: frontend test skipped)"; fi
python3 "$ROOT/tests/test_recomp.py" | tail -1
if python3 -c "import keystone" 2>/dev/null; then python3 "$ROOT/tests/test_arm7.py" | tail -1; else echo "(no keystone: ARM7 tests skipped)"; fi
