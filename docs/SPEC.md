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

### 2.3 Binary blobs — loader partly mapped  (`Item.dat`, `Component.dat`, `Sound.dat`, `Map/*.bfd`, `*.ape`)

Start with a `u32` count then dense binary with long single-byte runs. `Map/*.bfd` are
loaded through the `"%s.bfd"` format string (`0x4c6134`) inside the map loader
`FUN_00467bf0` (2872 bytes) — next target to decompile.

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
| `0x467bf0` | map (`.bfd`) loader |
| `0x49e03d` | path builder |
| `0x49e02a` | fopen wrapper |
| `0x49de96` | fread wrapper |
| `0x46bb90` | fatal error/log formatter |

## 4. Next analysis targets

1. `FUN_00467bf0` — `.bfd` map format (needed for the first walkable milestone).
2. Shape record body @ its data blob — vertices / UVs / skeleton binding (renderer).
3. Main loop / `WinMain` / entry; GL draw style (immediate vs. vertex arrays).
4. `WS2_32` send/recv call sites — multiplayer protocol → server reimplementation.
5. (optional) full PTX block decoder for a standalone PNG extractor.
