# dc-core

Shared foundation for native ports of Dreamcast games. It's used by
[rush2049-dc-port](https://github.com/jkod/rush2049-dc-port) and the Quake III port, and it serves
as the reference oracle for the Tennis 2K2 rebuild.

Nothing here contains game data. Every tool works on the player's own disc image.

## What's in it

| Path | What |
|---|---|
| `tools/disc/gdi.py` | GDI reader: track validation (names missing tracks), IP.BIN header, ISO9660 walk across all data tracks, extraction |
| `tools/recomp/sh4recomp.py` | SH-4 → C static recompiler: full integer + FPU ISA (PR/SZ/FR modes, XMTRX, fsca/fipr/ftrv), delay slots, call-target discovery, in-function jump tables |
| `runtime/` | CPU context, RAM/hardware access, dispatch table, HLE overrides, call hooks |
| `tools/assets/pvr.py` | PowerVR2 texture decoder (twiddled, VQ, small VQ, rect, palettised, mipmaps) → PNG |
| `tests/` | recompiler unit tests (hand-assembled SH-4), PVR round-trip tests |

## Recompiling a game

```sh
python3 tools/disc/gdi.py check  game.gdi
python3 tools/disc/gdi.py extract game.gdi out/ 1ST_READ.BIN
python3 tools/recomp/sh4recomp.py out/1ST_READ.BIN --funcs funcs.txt --out gen/
```

`funcs.txt` seeds function starts, for example from a Ghidra export. Call targets
(`bsr`, `mov.l @(pc) + jsr`) are discovered on top of that. Each recompiled function is
`void f_XXXXXXXX(sh4ctx *c)` and keeps the original calling convention, so the
original code can be called directly from C:

```c
sh4ctx c; dc_init(&c);
dc_load_binary("1ST_READ.BIN", 0x8C010000);
c.r[4] = arg; dc_call(&c, 0x8C02F478);         // run an original function natively
dc_hle_register(0x8C014D6A, my_native_sqrt);    // or replace one with your own code
```

In CMake, `dccore_add_recomp(game BIN path/1ST_READ.BIN FUNCS funcs.txt)` builds the
generated code as a static library.

## Status

- Tennis 2K2 (US v1.009): 7,018 functions recompile and build. The ball physics run
  natively and match the original to float rounding.
- Rush 2049 (PAL): waiting on a complete disc image.
- Next: PowerVR2 renderer (Vulkan/Metal/GL), AICA audio, Maple input, VMU saves, Switch build.

## Tests

```sh
python3 tests/test_recomp.py   # 16 SH-4 snippets: div1, delay slots, FPU modes, ftrv, jump tables...
python3 tests/test_pvr.py
```
