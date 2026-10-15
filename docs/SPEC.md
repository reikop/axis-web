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
needs **dynamic capture** — Frida-hook the texture-by-name loader `FUN_00456070` (`0x456070`)
on the running game to record each resolved `(name, glTextureId)` and tie it to the group.

**Dynamic-capture attempt (done, blocker precisely characterized).** AXIS was launched under
Frida on this host and the method is validated: the image loads at its preferred base
`0x400000` (no ASLR, so static addresses are runtime addresses), and `FUN_00456070` hooks
cleanly — confirmed to be *texture-lookup-by-name* (`param_1` = the texture-name C string; it
searches the six Image pages and errors `"can't find texture object '%s'"`). But **zero** calls
were observed through startup and the menu, because the client never loads a 3D scene: it stops
at a modal — *"게임서버를 선택하지 않았습니다. AxisPlay.exe를 실행후 게임서버를 먼저 선택해
주십시오"* ("no game server selected; run AxisPlay.exe and select a server first"). The scene
(map + models + textures, where `FUN_00456070` fires) only loads after that gate, and **this
install ships only `AXIS.EXE`** — no `AxisPlay.exe` launcher and no server config — while the
2005 servers are long dead. So the texture-binding capture needs the game first driven past the server-select gate.

**Gate bypassed + bindings captured (done).** The gate `FUN_0042b690` just reads the registry
key `HKCU\SOFTWARE\AxisArena\SelectServer` (values `TcpDNS`/`LadderDNS`/`TcpPort`/`LadderPort`,
normally written by AxisPlay.exe) — if present it proceeds, else it shows the error. Creating
that key with dummy values (127.0.0.1) lets the client boot normally — no binary patch needed.
The game then runs (screenshot-verified: the AXIS title / "INITIALIZING NETSPHERE" screen
renders, using `iftex/axis_title`), and the Frida capture (`port/frida/capture_textures.js`,
hooking `FUN_004658d0` + `FUN_00456070`) recorded the real bindings:

* **131 texture lookups, 59 distinct** on the boot/menu scene. UI comes from the `iftex/` pack
  (`axis_title`, `mainmenu01/02`, `arenamenu01/02`, `gameconnect`, `english8/16`, `han_cho`…).
* **Per-group 3D binding = one `Texture\NN` slot** — 55 shape render-objects each resolved a
  single `Texture\00` / `Texture\01` / `Texture\02` / `Texture\07` (a small material index
  formatted `Texture\%02d`), while item models use shape-relative paths
  (`Shape\item\warpzone\{warpzone,teleport_ani,cic_ani}`). This is why the static group+4 bytes
  read as junk: the real name is this short runtime-built `Texture\NN`, not stored whole.

So the binding scheme is known and the loader resolves a name straight into the six Image pages
(`FUN_00454f30`, every slot **PTX fmt 0**, `*` context prefix). **But a `Texture\NN` is a
per-context slot, not a fixed skin** — crucial caveat: the global `TEXTURE\00..09` entries in
`Image5.dat` are the **menu/default effects atlases** (sparkles, glows, UI buttons, a bullet — verified
by decoding them), *not* a given map's building textures. When a **map** loads it fills those
slots from its **own** textures, which live under `STAGE\<MAPNAME>\…` (e.g. `STAGE\METROPOLIS\`
has `METROPOLIS` (a skybox/backdrop panorama), `CLAN`, `SKYINNERLAYER`, `SKYOUTERLAYER`; other
stages add `SIDELAYER*`, `SPHERE*`). So `Texture\NN` only means the right thing *in that map's
context*. What **is** statically and correctly resolvable is a shape's **own skin**
`SHAPE\<name>` (e.g. `SHAPE\ALIEN\GIRL`, `SHAPE\ALIEN\ADD01`, `SHAPE\FINALLYFREE\TREE`, weapon
shapes) — those are real PTX entries the viewers decode (`ptx.js`, §2.2) and apply by UV, and
they are the textures `web/shapetex.html` and `map.html` use. The per-group numeric index `NN`
(stored in `group+4`) selects a per-map slot and is **not** resolved to the global `TEXTURE\NN`
(doing so paints generic effects atlases on buildings — a bug corrected here); correct map
geometry texturing needs the `STAGE\<map>\*` set plus the `NN`→`STAGE` slot mapping, which is a
per-map/context binding not recoverable from the static mesh alone.

**Where the name comes from (chain fully mapped).** `FUN_004658d0(param_1=texname, …)` iterates
the shape's groups (`count@shape+0x10`, array@`shape+0x14`, stride `0x80`, name@`group+4`, GL
id→`group+0x44`): when `param_1` is non-empty it overwrites **every** group's name with
`DAT_004b22cc`+`param_1` (one texture per shape-render); when empty each group keeps its own
`group+4`. Verified statically: `group+4` in the `.shp` is **empty / stale** (`''`, `' copy'`)
— the name is **not** in the mesh. `FUN_004658d0`'s only caller is the shape loader
`FUN_00466260`, so the texture name is **passed into the shape load**, decided by the caller:
* **items / map props** use a name derived from the shape's own path — e.g. the warpzone shape
  bound to `SHAPE\ITEM\WARPZONE\WARPZONE` (its own path), which *is* an `Image0.dat` entry;
* **mechs / parts** use the numbered loadout skins `Texture\NN` (from the component/loadout).
The `.bfd` map record carries only the object's shape path (no texture field), so for map props
the texture follows from that path. So textured rendering is: resolve each object's texture name
by type (path-derived for props, `Texture\NN` for mechs) → load that PTX from the Image pages
via `ptx.js` → apply. The name-derivation per type (and the loadout→`NN` mapping) is the bounded
remaining work; the decode and the binding mechanism are solved.
`port/frida/capture_textures.js` reproduces the capture (its header has the one-time registry
setup; remove with `reg delete "HKCU\SOFTWARE\AxisArena" /f`).

**Texture-load pipeline mapped (`FUN_00454f30`, the only caller of the PTX codec
`FUN_0046df80`).** It loads one texture slot: it takes the slot's stored name (at
`ecx + 0xc0128 + slot*0x68`), transforms it with the **context prefix `DAT_004b22cc`**
(`FUN_0049e29b`) into the real source name, then by a per-slot format flag (`ecx+0xc0114`)
loads it as **PTX** (from the Image pages — flag 0), **BMP** (flag 1) or **RAW** (flag 2),
building `_ALPHA` / `_COLOR` sibling names with `sprintf` (`"%s_ALPHA"`, `"%s_%s"`…). **Captured
live** (hooking `FUN_00454f30`): the context prefix `DAT_004b22cc` is a constant `*`, and each
slot's stored name is the **full** `TEXTURE\NN` / `IFTEX\…` / `SHAPE\ITEM\…`, all loaded as
**PTX (fmt 0)** — so the resolved source is exactly that name in the Image pages, which `ptx.js`
decodes. The alias→source question is thus answered: `Texture\NN` → the `TEXTURE\NN` PTX entry.

Until that mapping is wired in, the mesh/map viewers render untextured flat-shaded geometry and
the texture gallery (§2.2) shows the textures standalone.

### 2.10 Skeleton / animation (`Data/Skeleton.dat`, `class SkeletonManager`)

`Skeleton.dat` is the plaintext indexed archive (§2.1); each of its 232 entries is one
skeleton, loaded by `getSkeletonPtrBy()` (mirrors the shape loader, `0x4c6440`
"class SkeletonManager"). Per-entry layout (confirmed on `ARM\BITRON-BAAL-ARM`, 2420 B,
`LEG\…BOOATERLEG-LEG`, and all 232 rigs):

```
header:  i32 @0x10 = bone count (e.g. 10) ; also size@0x04, flags@0x18/0x1c ; sentinel saved ptrs
bone[boneCount], 0x70 (112) bytes each, first at +0x24:
    +0x00  char name[0x40]   NUL-terminated joint name  ("LSHO","LARM01","LARM02"…)
    +0x40  i32  parent       parent bone index, -1 = root
    +0x44  f32  tx, ty, tz    local translation from parent (= real bone lengths)
    +0x50  f32  angle         rotation angle, radians          \  axis-angle local
    +0x54  f32  ax, ay, az    unit rotation axis (|axis| ~= 1)  /  orientation
    +0x60  ...                runtime-only fields / saved pointers (clobbered in file)
<per-bone animation keyframe tracks follow>
```

The field split is **verified, not inferred**: the `parent` indices reconstruct exact,
sane hierarchies (ARM: LSHO→LARM01→LARM02→LFIST ×2; LEG: each LEG root → 3 foot nodes),
and the decode shows perfect **left/right mirror symmetry** — both arms give bone lengths
125.9 / 75.6 / 264.8, both legs' foot nodes land on one Y plane. Every bone's axis is
unit-length (`|axis|≈1.0`), confirming axis-angle. World bind-pose joints:
`world[b] = world[parent[b]] · Translate(t) · AxisAngle(angle, axis)`.

Tools: `tools/parse_skeleton.py` (parse + export per-rig JSON with precomputed world
joints), JS port `web/skel.js` (byte-exact, verified by `tools/verify_skel_js.js`),
viewer `web/skeleton.html` (orbitable bind-pose rig, live from Skeleton.dat).

**Animation keyframe region (structure decoded, cross-checked; playback semantics open).**
After the bind-pose bone array (`0x24 + bc*0x70`) each entry carries an animation region.
Verified against the bind pose used as ground truth (`tools/probe_skeleton_anim.py`):

* header `@0x1c` = **keyframe/sample count** — varies per rig (BAAL-ARM 30, BAAL-LEG 34,
  BAAL-BODY 6, NITRO-ARM 24); the region size scales with it, so the format is
  variable-length. `@0x18` = clip/animation count or flags (0,2,4,6…); `@0x20` = const 3300.
* The region is a ~68-byte header block followed by one **uniform-stride block per bone**
  (BAAL-ARM: `0x78` = 120 B/bone, confirmed identical across 9 of its 10 bones).
* Each bone block holds **axis-angle ROTATION keyframes** (angle + unit axis, 4 f32 each);
  keyframe 0 equals the bind-pose rotation *exactly* (matched on LSHO, LARM01, LARM02,
  LFIST, RSHO, RARM01, RARM02 — not a guess). The header block carries **root TRANSLATION
  keyframes** (the root joint's xyz changing frame to frame).
* int32 markers `1650` and `3300` (= 2×1650) delimit sub-structure (key times / indices).

Still **open** (needs the runtime animation sampler reversed or a dynamic trace to pin down
without guessing, the same bar as §2.9 texturing): the 1650/3300 time base, the exact
per-key record stride, the rotation/translation track interleave, and clip boundaries.
No guessed playback is shipped until these are verified.

The mesh's per-vertex `attr` (4 bytes, §2.8) is the likely bone-index/weight for skinning;
with verified keyframe playback + that skin binding the meshes can be deformed and animated.

### 2.11 Audio (`Data/Sound.dat`, `Sound/`, `Music/`)

The audio ships as **loose standard files**, not a packed archive: `Music/*.mp3` (23 named
tracks — "Valhalla", "Netsphere", "Speed of Light", …) and `Sound/**/*.wav` (SFX grouped by
category: weapon, getitem, alarm, boosters, impact, explode, interface). Both are directly
playable in a browser with HTML5 `<audio>`; no decoding needed. `Data/Sound.dat` (20 KB,
encrypted with the §2.3 cipher from offset 0) is the **sound-definition index**: decrypted it
lists 79 `Sound\<cat>\<clip>.wav` references plus per-definition params (volume/range fields
and an associated texture/item path). Tool: `tools/parse_sound.py` (decrypt + recover clip
list). Viewer: `web/audio.html` plays the music + SFX straight from the user's install (served
by `run.py` at `/game/`; `web/audio.json` holds only filenames — no audio is copied/committed).

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
   0x48de20, 0x48df60`) — the outbound message types. **Player-action packet
   (`FUN_0048d080`) — full 64-byte payload layout decoded from the builder and VALIDATED
   against the replays:**

   | off | type | field |
   |----:|------|-------|
   | 1   | u8   | packet type = **1** (player action) |
   | 2   | u8   | actor id (0..15) |
   | 3..8| u8×6 | appearance / colour (all `0xFF` while the slot is inactive) |
   | 9   | f32  | global aim/view A (`0x22f0408`) |
   | 13  | f32  | global aim/view B (`0x22f040c`) |
   | 17  | f32  | per-actor input A (`0x1eccb6c + actor*0x8888`) |
   | 21  | f32  | per-actor input B (`0x1eccb70 + actor*0x8888`) |
   | 25  | u16  | action/fire bitmask 1 (one bit per 0..15 channel, built in the 0x10 loop) |
   | 29  | u16  | action/fire bitmask 2 |
   | 33..42 | u16×5 | aim/angle shorts |
   | 43  | u8   | RNG stream selector (`counter % 5`) |
   | 44..55 | | RNG sync block — 5 draws from the shared table `DAT_00a11bc8[0x1000]` (idx `DAT_00a20488`) |

   The type-1 packet's four floats are small **normalised values (0..1)** (per-tick control
   inputs / aim), not world coordinates, and it also carries RNG samples from a shared table.

   **Packet-type catalogue — ALL 6 builders decoded, types validated on the replays.**
   Every builder writes the type to `payload[1]` then calls `pnSendPacketData(0x40,&buf)`.
   * **Type 0** — `FUN_0048d750`, the per-tick **STATE SNAPSHOT** and ~96% of all traffic:
     `[4]` = `(actor<<4)|mode` (so **actor = `[4]>>4`**), `[5]/[7]/[9]` = **int16 quantised
     world position x,y,z** (`__ftol` of the float coords), `[0xb..0x12]` orientation/velocity
     /weapon-fire flag bytes, then five 9-byte weapon/turret sub-records (flags + target id +
     3 aim shorts). **Verified: consecutive `[5]/[7]/[9]` samples form smooth world-scale
     trajectories** (e.g. actor 2: (931,−897,−5424)→(760,−854,−5415)→(675,−830,−5410)…).
   * **Type 1** — `FUN_0048d080`, player **INPUT/action event** (table above): normalised
     input axes + action/fire bitmasks + RNG sync block.
   * **Type 2** — `FUN_0048d580`, player **IDENTITY/loadout**: name string `[2..18]`, actor
     `[22]`, ~11 equipment ids `[23..33]` (+1 encoded), 20-byte stat/appearance block. (Real
     handles recovered from the replays: "OneShotOneKill", "onlynike", "efreet", "k'".)
   * **Types 3,4,5,6,9,13** — `FUN_0048db10` (a multiplexer draining control-event queues):
     short **notification/command** packets, `[2]`(+`[3]`) small params; several use the
     constant `[2]=0x6b`.
   * **Types 7,8,10,11** — also `FUN_0048db10`: **text/string messages** (chat / player name /
     clan), each `strcpy`s a NUL-terminated string into the payload after a 1–2 byte header.
   * **Type 12 (0xc)** — `FUN_0048de20`: **join/session bulk sync** — `[2]`=actor, `[3..4]`=
     cursor, `[5..]`=48-byte chunk (16×3 bytes) streamed from a per-actor 384-entry buffer,
     cursor advancing until exhausted (3× in the short replay, 27× in the longer ones).
   * **Type 14 (0xe)** — `FUN_0048df60`: **HP/status snapshot** — for up to 124 entities
     (stride 0x28) a clamped `value/10+1` (0..15) packed as a 4-bit **nibble**, two per byte,
     into a 62-byte field.

   **Architecture (corrected — this supersedes an earlier note that called it pure lockstep):**
   AXIS **replicates state**: each actor's quantised world position + weapon state is broadcast
   every tick (type 0). Control inputs and an RNG sync stream (type 1) ride on top for
   hit/damage determinism and anti-cheat, not full-simulation lockstep. The earlier "positions
   are never on the wire" reading was based on the type-1 packet alone; the dominant type-0
   packet does carry them, so **recorded movement is directly recoverable from a replay**.
   Consequence for the port: `server/relay.js`'s position-sharing model matches the original's
   type-0 state broadcast; recorded `.ape` matches can be replayed in the browser from type-0.

   **Key link:** the same 64-byte payload is also written to `AutoPlay\*.ape` via the
   encrypted write `FUN_0046c0b0` — so **`.ape` replays are recorded packet streams**
   (decryptable with §2.3). **Record format (verified):** a ~60-byte header then a stream of
   **76-byte records** `[u32 tick][u32 arg][u32 0x40][64-byte payload]` (12+64). All three
   `AutoPlay\*.ape` decrypt whole (version `0x1201`); each has **three real players** and a
   consistent type histogram — type **0** state/position (~96%), **1** input, **2** identity,
   **12** join-sync. Tool: `tools/parse_ape.py` (decodes type-0 trajectories + type-2 names +
   type census); JS port `web/ape.js` (byte-exact, `tools/verify_ape_js.js`) powers the
   in-browser match replay viewer `web/replay.html`.

   **Recv/dispatch** (`FUN_004a7ab0`): dequeues 64-byte packets from the per-channel ring
   buffers (`DAT_00af0b70`, 0x10 channels × 0x80 slots × 0x40 bytes), tracking per-channel
   read cursors (`DAT_00b12b70[ch]`, `& 0x7f`) and a round-robin channel index
   (`DAT_00b12bb0`, `& 0x0f`). It returns the packet **type** (1 = normal, 2 = relayed/echo
   with a re-send via `FUN_00450700`) plus the actor id and 64-byte payload. So both the
   **send** (`pnSendPacketData`) and **recv** (`FUN_004a7ab0`) sides of the transport are
   mapped; a `tools/`-side `.ape` replay decoder + the `server/relay.js` relay already
   implement this model end-to-end (verified two-client shared-map multiplayer in the browser,
   see `../server/README.md`). **All 6 outbound packet builders are now decoded** (the
   catalogue above), so the outbound protocol is fully mapped.

   **Inbound application (`FUN_0048bca0`), decoded — the counterpart to the builders.** The
   network tick `FUN_00497d70` drains packets in a loop: `recv (FUN_004a7ab0)` returns type
   1 (normal) → `FUN_0048bca0(payload,…)` applies it; type 2 (relayed) is re-sent inside recv.
   `FUN_0048bca0` is `switch(payload[1])` over the same types 0..14. The **type-0 apply**
   (verified, and it confirms the builder exactly) reads `actor = payload[4]>>4`, position
   int16 at `[5]/[7]/[9]`, then writes the actor's world transform with the real **unit scales**:
   * **position = int16 × 10.0** (so replay/`ape.js` coords are the raw int16 ×10 = true world units),
   * angles = `byte × 14.0625` wrapped to ±1800 (= ±180.0°): **`[0xc]` = yaw/heading**
     (full ±180°; verified — it tracks each player's movement direction `atan2(dx,dz)` to a
     ~13° median on actor 0, so the **ground plane is X–Z and Y is up**), `[0xb]` = pitch
     (±90°), `[0x10]` a further limited-range angle; velocity = `byte × 0.1`, trim nibbles × 1/15,
   * then the five 9-byte weapon/turret sub-records (flags, target id, 3 aim shorts ×10) — the
     same layout the builder emits. The **type-1 apply** nests `switch(payload[0x2b])` on the
     RNG stream selector, advancing the shared table — the determinism hook.
   So both directions of every packet type are mapped. Remaining for server-authoritative play
   is the rest of the per-tick simulation the dispatcher feeds (`FUN_004962d0`, `FUN_0047c480`,
   `FUN_00478f10`, `FUN_0048e220`, `FUN_004901d0`, `FUN_0048f1e0` — movement/physics/weapons/
   damage) — i.e. the game simulation itself, not the wire format.
5. (optional) full PTX block decoder for a standalone PNG extractor — done (`tools/ptx_decode.py`).

## 5. Simulation & determinism (entry points mapped)

The sim is the remaining multi-year pillar; its structure is now mapped so it can be
reconstructed function-by-function.

**Shared-RNG determinism (fully recovered + verified).** The game is deterministic lockstep for
*decisions* (spawns, AI, damage rolls): it draws from a **precomputed 4096-entry random pool**
`DAT_00a11bc8[0x1000]` through a single running index `DAT_00a20488`, as `pool[idx++ & 0xfff] % N`.
Every peer holds the same pool and index, so every randomized decision matches across clients —
which is why each packet also carries RNG samples (type 1, `[44..55]`) to keep the index in lockstep.
The pool is **loaded from `MATHFUNC.TBL`** (AXIS.EXE `FUN_0046b6f0`, plain `fread`, unencrypted):
the file is `float32 sin[3600]` + `cos[3600]` + `tan[3600]` (over 0.1° steps) + `uint32 rng[4096]`,
exactly 59584 bytes. **Decode verified** by matching the sin/cos tables to the real math functions
(max err ~1.7e-07) — and the RNG pool is exactly the **MSVC `rand()` LCG seeded with `srand(1)`**
(`seed = seed*214013 + 2531011; value = (seed>>16)&0x7fff`): regenerating it reproduces all 4096
shipped entries (4096/4096, first values 41,18467,6334,26500…). So the determinism RNG is fully
reconstructable offline. Tools: `tools/parse_mathfunc.py` (parse + verify); reconstructed as
`port/src/axis_rng.c` → `axis_rng_fill`, which the WASM build verifies 4096/4096 vs MATHFUNC.TBL.

**Round/level reset `FUN_0049d570`** (the `"incorrect level %d"` setup): on round start it draws
six values from the RNG pool to choose spawn points and round parameters (per-map spawn tables
at `0x15de6c8`, `0x15fff84`, `0x163b588`, `0x1679b34`, `0x16c5178`, `0x175522c`), copies the
spawn list, resets every actor's state block (stride `0x8888`), loads the level, and seeds the
view. This is where a match begins; it's RNG-driven so all peers spawn identically.

**Per-tick simulation**, driven each frame by the network tick `FUN_00497d70` after it applies
inbound packets (§4): `FUN_004962d0`, `FUN_0047c480`, `FUN_00478f10`, `FUN_0048e220`,
`FUN_004901d0`, `FUN_0048f1e0` — movement/physics, weapons/projectiles, collision, damage. Each
reads an actor's state block (`in_ECX + 0x1ecca70 + actor*0x8888`) whose fields the type-0 apply
writes (position `+0x1ac/1b0/1b4`, orientation `+0x1b8/1bc/1cc`, velocity, weapon sub-records).
These are the concrete functions to reconstruct next, each landing as a `port/src/*.c` module
(the pattern `axis_crypt.c` / `axis_netdecode.c` already follow) for the Emscripten/WASM build.

Behaviour-exact reconstruction of these + verification. **Update:** the **Emscripten toolchain
is now set up and working** (Ubuntu/WSL2, `emcc 3.1.69`, `node v22`) — `port/build.sh` compiles
the reconstructed C to a verified standalone WASM module (see `../port/README.md`), so the
*compiler* half of the old boundary is gone; the larger pieces (and these sim functions) can now
actually be built. What still blocks a behaviour-exact sim is **ground-truth to verify against**:
the per-tick functions only execute inside a live match, and this offline install cannot reach
one — re-tested 2026-10-16, a fresh `AXIS.EXE` stalls at the "INITIALIZING NETSPHERE" screen and,
even with a loopback listener up on `:7777`/`:7778`, makes **no `connect`/`socket`/DNS and no
game-level send/recv in 60 s** (the busy `WSAGetLastError` churn is all inside transient system
DLLs, never AXIS's own net code). So it blocks *before* the client's own connect logic — the
lobby/live-match, and with it the map building/terrain-texture binding (§2.9) and the animation
time-base (§2.10), stay unreachable by dynamic capture here. Those are the genuine remaining
frontier; the data-decode reconstructions (archive/PTX/.shp/.bfd) are instead verifiable offline
against the JS/Python references and are the natural next `port/src/*.c` modules.

See `../port/README.md` for how these feed the approach-B WASM build.
