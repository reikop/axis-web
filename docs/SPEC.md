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

**PTX is a custom JPEG** (confirmed by decompiling the codec):

- `FUN_0046de30(quality)` builds the **quantization table**:
  `quant[i] = (baseQuant[i] * scale[i] * 100) / quality >> 12`, where `quality` = the header
  `table` field (e.g. 300), `baseQuant` @`0x4c7d58`, `scale` @`0x4aba60` (0x200 bytes = 128
  ints: luma + chroma tables).
- `FUN_0046e510(stream)` inits the bit-reader (ptr @`0xa62bd8`, bit-pos `0xa22bd4`, DC
  predictor `0xaa3b5c`).
- `FUN_0046e530(block[64])` decodes one 8×8 block — **exactly JPEG entropy coding**:
  `DC = predictor + huffDC(); predictor = DC` (differential DC), then AC coefficients as
  `(run, value)` pairs (`huffAC`), zeros skipped by `run`, placed in **zig-zag order**
  (table @`0x4c6994`, 64 ints), terminated by EOB `(0,0)`.
- `FUN_0046e610` = DC huffman, `FUN_0046e720` = AC huffman (run/value).
- `FUN_0046d800` = **dequantize + 8×8 inverse DCT** + place into the plane.
- `FUN_0046dcb0`/`FUN_0046dd60` = combine planes **YCbCr → RGB/RGBA**.

**Implemented and verified** — `tools/ptx_decode.py` decodes PTX → PNG using the standard
JPEG zig-zag + luma/chroma quant tables and the two AXIS-specific Huffman tables read at
run time from the user's own `AXIS.EXE` (quant = `jpegQuant*100/quality`, plain 8×8 IDCT,
YCbCr→RGBA). Confirmed on real textures: a 64×64 panel decodes to the expected glowing
injection-port, and a 256×256 UI texture renders crisp readable text and imagery. Per-plane
full resolution (no chroma subsampling); planes = Y, Cb, Cr (+alpha for fmt 0x82); DC
predictor resets per plane; blocks are stored in raster-block order.

The zig-zag table @`0x4c6994` is the **standard JPEG zig-zag scan**, verified byte-for-byte:
`0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,
55,62,63`. So PTX is standard JPEG machinery under a custom header — a standalone decoder
is a baseline JPEG decoder fed the binary's quant/scale (`0x4c7d58`/`0x4aba60`) and huffman
tables, producing YCbCr planes combined to RGB/RGBA. All pieces are identified; this is the
next implementation task (and in approach B it compiles from the reconstructed C++ as-is).

### 2.3 Encrypted data files  (`Item.dat`, `Component.dat`, `Sound.dat`, `Map/*.bfd`, `*.ape`)

These are **obfuscated**, which is why they look like runs of `0x59`. The decrypting read
`FUN_0046c060` = plain `fread` (`FUN_0049de96`) **then per byte**:

```
b = rol8(b ^ 0x59, 1)        # XOR 0x59, then 8-bit rotate-left by 1
```

(`0x59 → 0`, so zero padding stores as `0x59`.) The transform is position-independent, so
decrypting a whole buffer uniformly equals the per-read decryption. The 4-byte **version
prefix** (`0x1201`, or `0x1204` for Component.dat) is read by the *plain* `FUN_0049de96`
and stays plaintext; the rest is encrypted. `Sound.dat` is encrypted from offset 0.
`Shape.dat` / `Skeleton.dat` / `Image*.dat` are **not** encrypted (plain `fread`).

Decoder + inspector: `tools/bfd_crypt.py`, `tools/parse_bfd.py`. Decrypted, the files are
fully legible, e.g. `Map/blocks.bfd` → `version 0x1201`, object count 10, ten 176-byte
object records (`block\object\b_upper_3_big`, …), then `PLAYER`, `STAGE/blocks`;
`Item.dat` → item names (`30mmShell_Box`, `Item\wapon\SRB`); `Component.dat` → mesh
component names (`MILLIA\HEAD`). `Item`/`Component` look like the same indexed-archive
shape as §2.1 but encrypted (exact record size still to confirm on decrypted bytes).

### 2.6 Map object placement  (decrypted `.bfd`, confirmed on all 40 maps)

After the plaintext `u32 version (0x1201)`, the decrypted stream begins with the object
placement list:

```
u32  section_size   (== count*0xB0 + 4)       # validates on all 40 shipped maps
u32  count
count × 0xB0 (176-byte) object records:
    +0x00  char[~0x88]  shape path name (NUL-terminated, cp949) — a Shape.dat entry
    +0x8c  f32[3]       position x, y, z   (confirmed by spatial spread across objects)
    +...   f32          orientation / scale (not yet identified)
```

Extractor: `tools/extract_map.py` → JSON; viewer: `web/map.html` (object layout in 3D).
The meshes referenced by name live in `Shape.dat` (mesh body format still to be decoded),
so this renders the map's spatial layout, not yet textured geometry. The heightmap-terrain
path (§2.5) exists in the engine but these arena maps are built from placed objects.

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
This is a standard renderable heightmap.

**Global header byte layout** (read from the assembly; `fread(dest,size,count,file)`):

```
u32   version  (== 0x1201)
4 ×   u32      global settings (each post-processed by FUN_0049df7e(file,val))
u8[256]        name / string block
u32            field @obj+0xa08
<sub-struct>   FUN_004831f0(obj+0x9ccffc, file)
u32, u8[16], u32, u8[16], u32, u32, u32, u32    lighting / sky / fog (16B = colour/vec)
u32   lightmap-mode @obj+0x9ee064  (==1 or ==2 → load that many named lightmap textures)
…     ground-texture set (5 names) + 6-tex + 16-tex sets, each loaded via FUN_00456070
u32   patch_count @obj+0xdc0a74
patch[patch_count]   (per-patch header read at loop top, then buffers computed)
```

**Per-patch read** (from assembly): after `u32 patch_count @obj+0xdc0a74`, each patch does
`fread(dest=obj+0xdc0244, size=0x20C, count=1)` — the **whole 524-byte patch struct in one
read** — then `fread`s its RGB heightmap block ((W+1)×(H+1)×3 bytes) into a ~197 KB stack
buffer (function reserves `0x30394` bytes) and converts R(==G==B) to a height byte.

Patch struct (int offsets, struct base = obj+0xdc0244; the first ~0x34 bytes are file data,
the rest are runtime pointer slots overwritten after load):

```
+0x00 i32 flip        +0x04..0x0c i32 ox,oy,oz (cast to float)
+0x1c i32 H (rows)    +0x20 i32 W (cols)
+0x24 i32 spacing     +0x28 i32 heightScale   +0x2c i32 uvTile
+0x34 … texture id/name fields;  +0x18.. runtime ptrs: height,flag,verts,normals,uv,index
```

Vertices: `x=col*spacing+ox, y=height*heightScale+oy, z=row*spacing+oz`.

**Complete global-header fread sequence** (only these consume file bytes; the many
`FUN_00456070` texture loads build a name and read nothing from the `.bfd`):

```
u32   version (0x1201)
4×u32 settings (each passed through FUN_0049df7e)
u8[256] name block
u32   @obj+0xa08
FUN_004831f0:  u32 n ; u32 ; n×44B records ; u32      (= 12 + 44·n bytes)
u32 ; u8[16] ; u32 ; u8[16] ; u32 ; u32 ; u32 ; u32   (lighting/sky/fog; @0x9ee030..064)
u32   6-tex-set count (@0x9ef0b0) ; u32 16-tex-set count (@0x9ef30c)
4×(u8[12])  +  3×u32                                  (@0x9ef904..93c; 12B = vec3)
u32   patch_count (@obj+0xdc0a74)
patch[patch_count]:  u8[0x20C] struct  +  u8[(W+1)(H+1)·3] RGB heightmap block
```

Everything needed for a byte-exact parser is now known except the exact size/stride of the
per-patch RGB block read (fixed 256-wide rows vs. packed (W+1)×3). **Next:** write
`tools/parse_bfd.py`, validate on the smallest map (version, sane n/patch_count/W/H, file
fully consumed), then feed the vertex/normal/UV/index buffers to a WebGL viewer.

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
| `0x49df7e` | fseek wrapper |
| `0x466200` | shape: fseek Shape.dat to an entry's mesh blob |
| `0x466260` | shape BST cache / loader (`getShapePtrByName`) |
| `0x464090` | **shape mesh file parser** (vertices/faces — next target) |
| `0x46bb90` | fatal error/log formatter |

### 2.7 Shape loading chain (toward real mesh geometry)

- `FUN_004660a0` loads the `Shape.dat` TOC; `FUN_0049df7e` = **fseek** (`fseek(f, off, origin)`).
- `FUN_00466200` = `getItemFile(name)`: looks up the index (`FUN_00466180`), `fseek`s
  `Shape.dat` to `TOC[idx].offset` (record+0x40), returns the positioned handle.
- `FUN_00466260` = `getShapePtrByName`: a **BST cache** of loaded shapes (node = 0x4c/76
  bytes: name[0x40], toc_index@0x40, left@0x44, right@0x48). On a miss it picks the format
  by `this+0x110` (0/1/2 → extension at `PTR_DAT_004c5cfc` / `DAT_004c5d00`; else "invalid
  file format") and calls the mesh parser.
- `FUN_00464090(name,off,size)` dispatches by extension: **`.shp`** (binary) → `FUN_004655c0`,
  **`.obj`** (Wavefront) → `FUN_00464250`. Shape.dat blobs are `.shp`.

### 2.8 .shp mesh format  (`FUN_004655c0`, Shape.dat is plaintext)

```
object header  0x4C bytes:
    +0x10 i32 groupCount      +0x20 f32[3] bbox-seed      +0x30 u32 tag (0x71110300)
groupCount × group header, 0x80 bytes each:
    +0x34 i32 vertexCount     +0x50 f32[6] bbox (min xyz, max xyz)
    (other group fields on disk are stale saved pointers, overwritten on load)
then per group, in order:
    positions  vc × vec3 (12B)     normals  vc × vec3 (12B)
    attr       vc × 4B             uv       vc × 2×f32 (8B)
    triangles  3 × u32 each        (read until indices leave [0,vc) — the empirical
                                    reader; the on-disk face-group count field is a
                                    stale pointer, same shift as vertexCount)
```

Verified: **948 / 948 single-group meshes parse with exact full byte-consumption**
(e.g. `ALIEN\GIRL` 491 v / 520 tris renders correctly in `web/shape.html`). Multi-group
meshes (795) need the inter-group padding pinned. Tools: `tools/parse_shp.py` (parse/inspect),
`tools/export_shapes.py` (→ JSON). The decompile's group offsets (vc@0x4c, fgc@0x60) are
shifted +0x18 from disk because the saved struct's pointer fields realign on load; the disk
offsets above (vc@0x34) are what the bytes actually use.

### 2.9 Mesh → texture binding (`FUN_004658d0`)

Each `.shp` group header stores a (partial) **texture name at group+4** (within the 0x80
header). After loading a shape, `FUN_004658d0` iterates the groups and, for each, builds the
full texture name by prepending a context prefix (`DAT_004b22cc` + the caller's `param_1`,
which varies by load context) to that stored name, loads it via `FUN_00456070`, and stores
the GL texture id at group+0x44.

**Static limitation (verified):** the name bytes at file group+4 are **fragmentary** —
across shapes they read as partial junk (`"GE"`, `" copy"`, `"opy"`, …), not whole texture
names — because they are stale remnants partly clobbered by the runtime pointer fields that
share the 0x80 group struct. No `texname → Image*.dat` formula matches (0 of 1899 tried). So
the full per-group texture name is **reconstructed at run time** from context and cannot be
recovered cleanly from the static `.shp` alone. Correctly texturing the meshes therefore
needs **dynamic capture** — e.g. Frida-hook the texture-by-name loader `FUN_00456070`
(`0x456070`) on the running game to record each resolved `(name, glTextureId)` and tie it to
the group — which first requires launching AXIS (its own effort, as it is an online title).
Until then the mesh/map viewers render untextured flat-shaded geometry and the texture
gallery (§2.2) shows the textures standalone.

## 4. Next analysis targets

1. Multi-group `.shp` inter-group padding → export the remaining 795 meshes; then wire
   real meshes into `web/map.html` (place each object's shape at its position).
2. `Skeleton.dat` + the `.shp` per-vertex attr/bone-weight → animation.
3. `.bfd` heightmap-patch byte layout (§2.5) for terrain maps that use it.
4. `WS2_32` protocol → server → online multiplayer; then the approach-B WASM build.
3. `WinMain` / entry + main loop; GL draw style (immediate `glBegin` vs. vertex arrays).
4. **Network (multiplayer)** — `WS2_32` is imported by **ordinal** (so `recv`/`send` don't
   resolve by name); connection strings in the main EXE: `"iftex/gameconnect"` (`0x4ae56c`),
   `"%s has been connected."` (`0x4c5608`), `"The network connection lost. The game ends."`
   (`0x4c58f8`). Much of the socket/server-list logic lives in `Launcher/` (`GetList.exe`,
   `SocketUpdate/`, `ServerList.dat`). Reversing the packet format + dispatch and
   reimplementing the server is its own large workstream (→ Node server + WebSocket relay).

   **Packet format** (from `pnSendPacketData` = `FUN_004a79c0`): game packets carry a
   **fixed 64-byte (0x40) payload** — a non-0x40 size triggers the `pnSendPacketData()`
   size error. The payload is wrapped with a **9-byte header** and sent as a **73-byte
   frame** (`FUN_00450700(buf, 0, payload+9)`). A per-channel **sequence number** (channel
   id `DAT_009fee24`; counters `DAT_00b12b70[ch]`, wrapped `& 0x7f`) indexes a resend ring
   buffer at `DAT_00af0b70` (0x80 slots × 0x40 bytes per channel). Next: the 9-byte header
   fields, the socket send (`FUN_00450700`), and the recv/dispatch (packet-type → handler).

   **Six send sites** call `pnSendPacketData` (`0x48d080, 0x48d580, 0x48d750, 0x48db10,
   0x48de20, 0x48df60`) — the outbound message types. The player-action packet
   (`FUN_0048d080`) fills a 64-byte struct: `[+1]=1`, `[+2]=actor id`, team/colour bytes,
   `[+0x09]` position (2×f32), orientation shorts, per-slot fire/action bitmasks
   (`local_27`/`local_23`), plus RNG-table "checksum" words (anti-cheat). **Key link:** the
   same 64-byte payload is also written to the `AutoPlay\*.ape` file via the encrypted write
   `FUN_0046c0b0` — so **`.ape` replays are recorded packet streams** (decryptable with
   §2.3), giving real protocol samples without a live server. **Validated:**
   `Autoplay001.ape` (539 KB) decrypts whole (version `0x1201`) and contains the per-tick
   64-byte player records (tick / actor id / payload with position). Decoding a full `.ape`
   is the practical way to recover the packet semantics for the server reimplementation.
5. (optional) full PTX block decoder for a standalone PNG extractor — done (`tools/ptx_decode.py`).

See `../port/README.md` for how these feed the approach-B WASM build.
