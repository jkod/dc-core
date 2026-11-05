# dc-core

Shared foundation for native ports of Dreamcast games.

Nothing here contains game data. Every tool works on the player's own disc image.

## What's in it

| Path | What |
|---|---|
| `tools/disc/gdi.py` | disc reader for `.gdi`, Redump `.cue` and `.chd`: track validation (names missing tracks), IP.BIN header, ISO9660 walk across all data tracks, extraction |
| `tools/disc/chd.py` | pure-Python CHD (v5) reader for CD/GD-ROM images, so setup needs no chdman (the runtime reads CHDs through libchdr) |
| `tools/recomp/sh4recomp.py` | SH-4 → C static recompiler: full integer + FPU ISA (PR/SZ/FR modes, XMTRX, fsca/fipr/ftrv), delay slots, call-target discovery, in-function jump tables |
| `runtime/` | CPU context, RAM/hardware access, dispatch table, HLE overrides, call hooks |
| `tools/assets/pvr.py` | PowerVR2 texture decoder (twiddled, VQ, small VQ, rect, palettised, mipmaps) → PNG |
| `tools/textures/upscale.py` | texture packs: upscales a port's texture dump with Real-ESRGAN (GPU program or CPU) — see below |
| `tools/steam/steam_shortcut.py` | adds a built port to Steam as a non-Steam game, with library art — see below |
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

Every tool and the runtime take a `.chd` wherever they take a `.gdi` (CHDs made with `chdman
createcd`, from a `.gdi` or a `.cue`). The runtime reads them through
[libchdr](https://github.com/rtissera/libchdr) (BSD-3-Clause), fetched by CMake at a pinned commit;
`-DDCCORE_CHD=OFF` builds without it.

### Found functions: each run makes the next build faster

Code the function list missed still runs, through the interpreter. The runtime notes each such
address in the executable once, in `dc_found_funcs_path` (the frontend puts `found_funcs.txt` next
to the VMU saves; `DC_FOUND_FUNCS=path` sets it anywhere). Give that file to the build and those
functions are recompiled too:

```cmake
dccore_add_recomp(game BIN path/1ST_READ.BIN FUNCS funcs.txt FOUND ${CMAKE_CURRENT_SOURCE_DIR}/found_funcs.txt)
```

(`sh4recomp.py --extra-funcs FILE` does the same by hand; a missing file is skipped.) Copy the
frontend's file into the port, rebuild, and the code that was interpreted runs natively.

## Texture packs

The GL renderer can replace any texture with a PNG of any size. Textures are named by a hash
of their decoded pixels: `<hash>_<w>x<h>.png`.

- `dc_pvr_gl_tex_dump = "dir"` saves every texture the game uploads into `dir`, once each
  (ports: `--dump-textures`). Play through the parts of the game you want covered.
- `dc_pvr_gl_tex_pack = "dir"` uploads `dir/<name>.png` in place of the texture of that name
  (ports: a `textures` folder next to the program). Missing files fall back to the original.

Or extract them from the disc, without playing (`dccore/texpack.h`; ports: `--extract-textures`):
`dc_tex_extract_disc("dir", unpacker, user)` walks every file on the disc, decodes every PVRT
texture it finds (Sega's standard format, on its own or inside archives) with the renderer's own
decoder, so the names match the dumped ones, and hands each file to the port's unpacker for the
game's own containers. An unpacker passes raw PVR data to `dc_tex_extract_pvr` (format as a texture
control word) or decoded pixels to `dc_tex_extract_argb`. Paletted textures get their colours from
the game while it runs, so they are skipped: dumping by playing still catches those, and anything
the game draws or recolours at run time. Both write the same names, so both can fill one folder.

To make a pack, upscale the dump:

```sh
# GPU (fast): realesrgan-ncnn-vulkan from https://github.com/xinntao/Real-ESRGAN/releases
python3 tools/textures/upscale.py textures_dump textures --exe path/to/realesrgan-ncnn-vulkan --models path/to/models
# CPU (slow): pip install ncnn pillow numpy, models/ from the same release
python3 tools/textures/upscale.py textures_dump textures --models path/to/models
```

Each texture is wrapped at its edges before upscaling so tiling stays seamless, and on/off
alpha (cut-outs) stays on/off. Any PNG editor works for hand fixes: keep the file name.
A pack is tied to the game's texture data, never to a dc-core version, as long as the
texture decoder keeps decoding the same pixels.

## Rumble (Jump Pack)

Each controller has a Jump Pack (Puru Puru Pack) in its second slot, so games that support it
find it and send their effects. `dc_rumble_level(port, &low, &high)`, called once per field, turns
the current effect into levels for the host's two motors: power 1-7 sets the strength, rising and
falling effects ramp over their pulses, and slow pulses (4 Hz) use only the big motor while fast
ones (30 Hz) add the small one. The frontend sends this to the gamepad on that port through SDL;
`no_rumble` in its config, or `DC_NO_RUMBLE=1`, leaves the packs out. In the on-screen menu,
*Rumble pad X* sets the strength for the controller that opened it (off, low, normal, strong; a
short buzz shows the new setting), kept per port in `osd.cfg`. Continuous vibration stops
after 5 s if the game never sends its stop.

## Add to Steam

```sh
python3 tools/steam/steam_shortcut.py add --exe build/rush2049.exe --name "San Francisco Rush 2049" \
        --portrait cover.png --hero hero.png --logo logo.png --tag Dreamcast
python3 tools/steam/steam_shortcut.py remove --exe build/rush2049.exe --name "San Francisco Rush 2049"
python3 tools/steam/steam_shortcut.py list
```

Writes the shortcut to every Steam user's `shortcuts.vdf` (Windows, macOS, Linux and Steam Deck,
Flatpak Steam included; `--steam DIR` / `--user ID` to choose) and puts the art under the id Steam
gives the shortcut. Adding again updates it; other shortcuts and unknown fields are kept, and a
`.bak` is written first. Close Steam before running it: Steam rewrites the file when it exits.

## Status

- Next: PowerVR2 renderer (Vulkan/Metal/GL), AICA audio, Maple input, VMU saves, Switch build.

## Windows CE games

Some Dreamcast games run on Windows CE: the disc boots `0WINCEOS.BIN` and the game is an EXE plus
DLLs instead of a `1ST_READ.BIN` (Sega Rally 2, for example). dc-core runs them without the CE kernel
(`dccore/wince.h`, library `dccore_wince`):

```sh
python3 tools/wince/ce_layout.py moddir --exe GAME.EXE --dlls COREDLL.DLL OLE32.DLL ... --out layout/
python3 tools/recomp/sh4recomp.py layout/image.bin --base $(cat layout/base.txt) --funcs layout/funcs.txt --no-discover --out gen/
```

`ce_layout.py` gives every module a fixed address (the EXE at its own base, the DLLs packed after it,
relocated) and lists the functions from each module's `.pdata` (start and length: exact on SH-4).
At run time `ce_init` maps the same files from the disc to the same addresses in a 32 MB user address
space (`dc_uram`, virtual 0x00000000.., also reached through its P1 fold 0x80000000.. where recompiled
code addresses its own instructions) and `ce_run` starts the EXE. The disc's own COREDLL runs
recompiled (C runtime, `wsprintf`, helpers); its kernel and window-manager calls are native: threads on
fibers (`dc_fiber_*`) with CE's priority scheduling and preemption at interrupts, events, mutexes,
semaphores, critical sections, TLS, heaps and `VirtualAlloc`, `VirtualCopy` as address windows onto the
hardware (`dc_hw_map`), interrupts as events (`InterruptInitialize`), files on the disc (`\CD-ROM\...`),
WDM devices (`\Device\...` through WDMLIB), a registry, window messages, and `CoCreateInstance` through
each DLL's `DllGetClassObject`. `DC_CE_TRACE=1` logs the calls.

## Tests

```sh
python3 tests/test_recomp.py   # 16 SH-4 snippets: div1, delay slots, FPU modes, ftrv, jump tables...
python3 tests/test_pvr.py
```
