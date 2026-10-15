# AXIS — Reverse-Engineering Spec (work in progress)

Living notes for the web port. Everything here is derived from static analysis of
a legally-owned copy of `AXIS.EXE` (2005 Korean 3D action game). No original code
or assets are reproduced in this repository.

- Target: `AXIS.EXE`, PE32 / x86, image base `0x400000`
- SHA-256: `7a8bc228a1f0a465c438153274cdc94de0cb6b0ee54e0d12347b08f3dfa2fbb9`
- 1826 procedures, 6 sections, ~1140 defined strings

## 1. Platform / API surface (what the web shim must cover)

Imported DLLs (from the import table):

| DLL | Role | Web shim target | Difficulty |
|-----|------|-----------------|------------|
| `OPENGL32`, `GLU32` | 3D rendering (fixed-function GL) | WebGL via Emscripten GL layer | low ✅ |
| `DSOUND` | sound effects | WebAudio | medium |
| `WINMM` | MP3 music, timers | WebAudio / perf.now | low |
| `DINPUT` (`DirectInputCreateA`) | keyboard/mouse | DOM input events | low |
| `WS2_32`, `ICMP` | multiplayer sockets, ping | WebSocket proxy (Emscripten BSD sockets) | medium |
| `KERNEL32`/`USER32`/`GDI32`/`SHELL32`/`ADVAPI32`/`ole32` | Win32 platform | Emscripten/SDL | low |

**Key finding:** rendering is **OpenGL**, not Direct3D. This makes the Emscripten
→ WebGL path realistic; Emscripten translates GL/GLES calls to WebGL and can emulate
legacy fixed-function GL (`-sLEGACY_GL_EMULATION`).

## 2. File formats

### 2.1 Indexed archive  (`Data/Shape.dat`, `Skeleton.dat`, `Image0.dat`, …)

Confirmed from the Shape.dat loader `FUN_004660a0` @ `0x4660a0`:

```c
fread(&count, 4, 1, f);          // u32 entry count
buf = operator_new(count * 0x44); // 0x44 = 68 bytes per record
fread(buf, 0x44, count, f);      // the TOC
// entry data blob follows immediately after the TOC
```

Layout:

```
offset 0x00 : u32  count
offset 0x04 : record[count]      // each 0x44 (68) bytes
                char name[0x40]  // NUL-terminated path (backslash dirs, cp949),
                                 //   trailing bytes hold extra fields/labels (TBD)
                u32  data_offset // absolute file offset of this entry's data
<data blob>                      // entry i spans [offset_i, offset_{i+1})
```

Verified against `Shape.dat`: count=1743, TOC end = `4 + 1743*68 = 118528`,
first `data_offset` = 118528 (exact match); all offsets monotonic and in range.
Parser: `tools/parse_archive.py`.

**TODO:** confirm the exact sub-fields inside `name[0x40]` (base name vs. node/bone
labels) and whether `Skeleton.dat` / `Image*.dat` share the identical record size.

### 2.2 Binary blobs — loader TBD  (`Item.dat`, `Component.dat`, `Sound.dat`, `Map/*.bfd`, `*.ape`)

Start with a plausible `u32` count (`0x1201`, `0x1204`, …) then dense binary with
long runs of a single byte (`0x59`/`0x49`/`0xcf`). Could be an XOR/obfuscation layer
over zero-padded regions, or raw packed mesh data. **Must read each loader** to decide.
`Map/*.bfd` are loaded via the `"%s.bfd"` format string @ `0x4c6134`.

## 3. Reverse-engineering map (procedures found)

| Address | Name | What |
|---------|------|------|
| `0x4660a0` | shape index loader | opens `Data\Shape.dat`, reads TOC |
| `0x49e03d` | path builder | prefixes relative path |
| `0x49e02a` | `fopen` wrapper | |
| `0x49de96` | `fread` wrapper | |
| `0x46bb90` | error/log formatter | printf-style |

## 4. Open questions (next analysis targets)

1. Loader for the `.bfd`/`Item`/`Component` blobs → §2.2.
2. GL usage pattern: immediate mode vs. vertex arrays? (drives renderer design)
3. Main loop / entry point structure.
4. Network protocol (WS2_32 send/recv call sites) → multiplayer server.
5. Shape record body format (vertices, UVs, skeleton binding).
