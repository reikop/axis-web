# AXIS — Reverse-Engineering Spec (work in progress)

Living notes for the web port. Everything here is derived from static analysis of
a legally-owned copy of `AXIS.EXE` (2005 Korean 3D action game). No original code
or assets are reproduced in this repository.

- Target: `AXIS.EXE`, PE32 / x86, image base `0x400000`
- SHA-256: `7a8bc228a1f0a465c438153274cdc94de0cb6b0ee54e0d12347b08f3dfa2fbb9`
- 1826 procedures, 6 sections, ~1140 defined strings
- Chosen approach: **B** — reconstruct C++ → Emscripten → WASM, with a WebGL/WebAudio/
  WebSocket platform shim. Rendering is OpenGL (not D3D), which makes this viable.

## 1. Platform / API surface (the shim the WASM build must provide)

Imported DLLs:

| DLL | Role | Web shim target | Difficulty |
|-----|------|-----------------|------------|
| `OPENGL32`, `GLU32` | 3D rendering (fixed-function GL) | WebGL via Emscripten GL layer | low ✅ |
| `DSOUND` | sound effects | WebAudio | medium |
| `WINMM` | MP3 music, timers | WebAudio / perf.now | low |
| `DINPUT` (`DirectInputCreateA`) | keyboard/mouse | DOM input events | low |
| `WS2_32`, `ICMP` | multiplayer sockets, ping | WebSocket proxy (Emscripten BSD sockets) | medium |
| `KERNEL32`/`USER32`/`GDI32`/`SHELL32`/`ADVAPI32`/`ole32` | Win32 platform | Emscripten/SDL | low |

Confirmed GL symbols in use: `glGenTextures`, `glTexImage2D` (texture upload path).

## 2. File formats

### 2.1 Indexed archive  (`Data/Shape.dat`, `Skeleton.dat`, `Image0.dat`..`Image5.dat`)

Confirmed from the Shape.dat loader `FUN_004660a0` and the texture-cache init `FUN_00454510`:

```
offset 0x00 : u32  count
offset 0x04 : record[count]      // each 0x44 (68) bytes
                char name[0x40]  // NUL-terminated path (backslash dirs, cp949);
                                 //   trailing bytes may hold extra labels/fields
                u32  data_offset // absolute file offset of this entry's data
<data blob>                      // entry i spans [offset_i, offset_{i+1}), last → EOF
```

Loader reads it as: `fread(&count,4,1,f); buf = new(count*0x44); fread(buf,0x44,count,f)`.

Verified:
| file | entries | TOC end = 1st offset |
|------|--------:|----------------------|
| `Shape.dat`    | 1743 | 118528 ✓ |
| `Image0.dat`   |  238 | 16188 ✓ |
| `Skeleton.dat` |  232 | 15780 ✓ |

Parser: `tools/parse_archive.py` (and JS port in `web/`).

### 2.2 PTX texture  (payload of every `Image*.dat` entry)

Each entry's data is a **PTX** blob. Parser: `FUN_0046df80` (validates `"Not PTX file"`).

Header (16 bytes, `fread(hdr,1,0x10,f)`):

```
+0x00  char[4]  magic   "PTX\0"
+0x04  u16      version  == 0x0100 (required)
+0x06  u16      w8       width  = w8  * 8
+0x08  u16      h8       height = h8  * 8
+0x0a  u16      format   0x81 => 3 planes (RGB),  0x82 => 4 planes (RGBA)
+0x0c  u16      table    passed to codec-init FUN_0046de30 (quant/huffman table id)
+0x0e  u16      (pad)
```

Verified on `Image0.dat` entry 0 (`SHAPE\ASSAULT\BACK`): `50 54 58 00 | 0001 | 0800 | 0800 | 8200 | 2c01`
→ 64×64, RGBA, table=0x12c.

Decode pipeline (planar, 8×8-block entropy codec — JPEG-like):

```
planes = (format==0x81 ? 3 : 4)
codec_init(table)                              # FUN_0046de30
read 0x10000 bytes of compressed stream        # per-plane bitstreams, byte-aligned
for p in 0..planes-1:
    mode = (p in {1,2}) ? 1 : 0                # chroma planes use mode 1  (YCbCr-ish)
    bitreader_init(stream + plane_byte_off)    # FUN_0046e510
    for block in ceil(w*h / 64) blocks:        # 8x8 = 64 px blocks
        bits = decode_block(tmp64)             # FUN_0046e530  (entropy + inverse xform)
        place_block(tmp64, plane_buf+p*0x10000)# FUN_0046d800  (8x8 -> plane raster, +0x40)
    plane_byte_off += ceil(bits_consumed / 8)
combine_planes -> interleaved RGB/RGBA         # FUN_0046dcb0 (RGB) / FUN_0046dd60 (RGBA)
emit(out, w, h, planes)                        # FUN_0046deb0  -> glTexImage2D buffer
# returns GL format code: 2 (RGB) or 3 (RGBA)
```

**For approach B this codec is reconstructed to C++ and compiled as-is** — no need to
re-derive the DCT/entropy math in script. A standalone PNG extractor (optional, stage 1)
would require reversing `FUN_0046e530` + `FUN_0046de30` fully.

### 2.3 Binary blobs  (`Item.dat`, `Component.dat`, `Sound.dat`, `Map/*.bfd`, `*.ape`)

Start with a `u32` version/count then dense binary. **File version = `0x1201`** (the
`01 12 00 00` header seen on `.bfd` and `.ape`; checked in the autoplay path and against
`DAT_009dbd44`, else `"file damaged"` / `"incorrect file version"`).

### 2.4 Map load pipeline  (`FUN_00467bf0`, the loading-screen state machine)

A map is `<name>.bfd`, opened in sequence by several section parsers (submode at
`this+0x233cc24`); the loading-screen labels name each stage:

| stage | parser | reads from `.bfd` |
|-------|--------|-------------------|
| LOAD_ZONE_INFORMATION | `FUN_0048ae40` | ground / terrain (**first walkable-map target**) |
| LOAD_OBJECT / OBSTACLE | `FUN_0048ab80(buf,0)` | static objects & obstacles |
| SET_COLLISION_VOLUME | `FUN_004704c0` | collision volumes |
| analyze_battle_field / play_info | `FUN_00486b30` | spawn / battle-field data |
| mission objectives | `FUN_0048aa30` | objectives |
| load_actor_info | `FUN_004739d0` | actors / NPCs |

Autoplay/replay (`AutoPlay\%s`, mode 3) reads the same `0x1201` container. The tick also
draws the loading screen with GL (`glClearColor`/`glClearDepth`/`glClear`/`glColor4f`).

### 2.5 Terrain / ground  (`FUN_0048ae40`, the LOAD_ZONE parser)

The ground is **heightmap terrain**: a global header (version `0x1201`, lighting/sky/fog
settings, up to 5 named ground textures + a 6-tex and a 16-tex set, each loaded by name via
`FUN_00456070`) followed by `N = *(this+0xdc0a74)` **terrain patches** (per-patch struct
stride `0x20C` bytes, array at `this+0xdc0264`).

Per patch (field = int index off the patch pointer `p`):

| field | meaning |
|-------|---------|
| `p[0]=W`, `p[-1]=H` | grid is `(W+1)×(H+1)` vertices |
| `p[-7..-5]` | origin offset (ox, oy, oz) |
| `p[1]` | cell spacing | `p[2]` | height scale | `p[3]` | UV tile size |
| `p[-8]` | normal-flip flag | `p[5]` | patch texture (loaded by name) |
| `p[6]` | heightmap: 1 byte/cell (grayscale value where R==G==B in the source RGB block) |
| `p[7]` | per-cell flag/mask (neighbour-equality test) |
| `p[8]` | **vertices** f32[3]/cell: `x=col*sp+ox`, `y=height*hs+oy`, `z=row*sp+oz` |
| `p[9]` | **normals** f32[3]/cell (cross products via `FUN_00466e40`, negated if `p[-8]==1`) |
| `p[0xc]` | **UVs** f32[2]/cell: `vertex.x/tile`, `vertex.y/tile` |
| `p[0xe]`,`p[0x11]` | index / strip buffers |

All buffers are bump-allocated into one arena at `this+0xdc0264+...` (cap 4,000,000 B).
This is a standard renderable heightmap. **Exact .bfd byte order of the header + per-patch
header still needs the `fread` (FUN_0046c060) argument sizes from the assembly** — the
decompile shows the sequence but not each field's width.

## 3. Reverse-engineering map (procedures)

| Address | Role |
|---------|------|
| `0x4660a0` | Shape.dat index loader |
| `0x454510` | texture-cache init (picks `Image*.dat` by resolution, `glGenTextures` slot ring, 0x14-byte slots) |
| `0x455e60` | material setup — 6 size layers: 64², 128×64, 256×64, 128², 256×128, 256² |
| `0x46df80` | **PTX decoder** (header + planar block pipeline) |
| `0x46e510` | PTX per-plane bitreader init |
| `0x46e530` | PTX 8×8 block decode (entropy + inverse transform) |
| `0x46d800` | PTX 8×8 block → plane raster placement |
| `0x46de30` | PTX codec/table init (arg = header `table`) |
| `0x46dcb0` / `0x46dd60` | PTX plane combine → RGB / RGBA |
| `0x46deb0` | PTX emit to output buffer |
| `0x467bf0` | map load state machine (loading screen) |
| `0x48ae40` | `.bfd` ground / terrain parser (next target) |
| `0x48ab80` | `.bfd` object / obstacle parser |
| `0x486b30` | `.bfd` battle-field / play-info parser |
| `0x48aa30` | `.bfd` mission-objectives parser |
| `0x4739d0` | `.bfd` actor-info parser |
| `0x456070` | texture load-by-name (`Texture\...`) |
| `0x49e03d` | path builder |
| `0x49e02a` | fopen wrapper |
| `0x49de96` | fread wrapper |
| `0x46c060` | fread wrapper (used by `.bfd` parsers) |
| `0x466e40` | vector/cross-product helper (terrain normals) |
| `0x456070` | texture load-by-name |
| `0x46bb90` | fatal error/log formatter |

## 4. Next analysis targets

1. `.bfd` exact byte layout — read the `FUN_0046c060` (fread) arg sizes in `FUN_0048ae40`'s
   assembly to pin each header + per-patch field width, then write a terrain parser + viewer.
2. Shape record body @ its data blob — vertices / UVs / skeleton binding (renderer).
3. `WinMain` / entry + main loop; GL draw style (immediate `glBegin` vs. vertex arrays).
4. `WS2_32` send/recv call sites — multiplayer protocol → server reimplementation.
5. (optional) full PTX block decoder for a standalone PNG extractor.

See `../port/README.md` for how these feed the approach-B WASM build.
