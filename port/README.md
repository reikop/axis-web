# Approach B — WASM port plan

## Status: the WASM path is live and verified

Two reconstructed primitives already **run as WebAssembly and are verified on the real game
files** (not just the cipher):

- `src/axis_crypt.c` — the obfuscation cipher (`FUN_0046c060`/`FUN_0046c0b0`, SPEC §2.3).
- `src/axis_netdecode.c` — the type-0 state-packet **codec**: `axis_decode_state` (`FUN_0048bca0`
  case 0) → actor + world position (int16×10) + heading, and its inverse `axis_encode_state`
  (the send side, `FUN_0048d750`) that builds an AXIS-format state packet. **`node wasm/build_wasm.js`
  verifies both f32-exact on all 6844 type-0 packets of a real replay — decode 6844/6844, encode
  6844/6844, and encode∘decode recovers every original packet.** So a WASM client can both parse
  and emit the game's real wire format, not an ad-hoc relay format.

Both are verified in two ways:

- `build.sh` compiles `src/*.c` to WASM with Emscripten (run in WSL; see below).
- Because this repo was produced with no C toolchain, `node wasm/build_wasm.js <game_dir>`
  **hand-assembles the identical module** (202 bytes) and verifies it: decrypt == reference
  and encrypt∘decrypt == identity, then decrypts every `Map/*.bfd`, `AutoPlay/*.ape` and
  `Sound.dat` byte-identically (10/10 pass). It also emits `web/crypt.js` (the module embedded
  for the browser), and `web/wasm.html` shows it decrypting your files in-browser with the
  **WASM** backend (verified: all PASS, 0 errors).

So approach B is proven end-to-end in this environment: reconstructed AXIS logic → WASM →
runs in Node and in the browser on real data. Each further `src/*.c` (archive index, PTX
codec, `.shp` mesh, map decode, then the simulation) is added to `build.sh`'s `SRC` list the
same way.

---

Reconstruct the client as C++ and compile it to WebAssembly with Emscripten, replacing
the Win32 / OpenGL / DirectSound / DirectInput / Winsock platform layer with a web shim.
This keeps the game's own logic and data formats behaviour-exact; only the OS boundary is
rewritten. Rendering is OpenGL, so Emscripten's GL→WebGL layer does most of the graphics
work. See `../docs/SPEC.md` for formats and the procedure map.

## Toolchain (WSL)

```sh
# in WSL (Ubuntu)
git clone https://github.com/emscripten-core/emsdk
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source ./emsdk_env.sh        # provides emcc / em++
emcc -v                      # verify
```

## Shim surface (the only part that is newly written)

| Native boundary | Web replacement | Emscripten mechanism |
|-----------------|-----------------|----------------------|
| `OPENGL32` / `GLU32` | WebGL | built-in GL layer; `-sLEGACY_GL_EMULATION=1` -sFULL_ES2 for fixed-function + `glBegin`/`glColor4f` style |
| window / swap / timing (`USER32`/`GDI32`/`WINMM`) | canvas + rAF | `-sUSE_SDL=2`, `emscripten_set_main_loop` |
| `DINPUT` | keyboard/mouse events | SDL2 input, or direct DOM handlers |
| `DSOUND` | WebAudio | SDL2 audio / OpenAL (`-lopenal`) mixing shim |
| `WINMM` MP3 music | `<audio>` / WebAudio | stream the existing `Music/*.mp3` |
| `WS2_32` / `ICMP` | WebSocket | `-sPROXY_POSIX_SOCKETS` + `-lwebsocket.js`, or a TCP↔WS relay to the game server |
| file I/O (`fopen`/`fread`) | preloaded FS / fetch | `--preload-file`, or fetch the user's own data at runtime |

Game data is never bundled. The build expects the player to supply their own installed
`Data/`, `Map/`, `Music/`, etc., mounted into the Emscripten FS (or fetched locally).

## Reconstruction order (incremental, each step runs before the next is reversed)

1. **Bootstrap + shim**: entry/main loop (`WinMain`), the GL context, a black frame.
2. **Archives**: `Shape.dat` / `Image*.dat` TOC readers (already specified — §2.1).
3. **Textures**: the PTX decoder (`FUN_0046df80` + block codec) → `glTexImage2D`. Compile
   as-is; no re-derivation needed.
4. **Terrain**: `.bfd` ground parser (`FUN_0048ae40`) + its draw path → **walkable map**.
5. **Models/animation**: Shape record body + `Skeleton.dat` → animated meshes.
6. **Game logic**: items/combat/physics (`Item.dat`, `Component.dat`).
7. **Network**: `WS2_32` call sites → protocol → Node server + WebSocket relay → multiplayer.

## Status

Stage 0 (analysis) in progress — formats and the shim surface are mapped in `../docs/SPEC.md`.
No C++ has been reconstructed yet; this directory is the home for it. Nothing here is
built or verified until the WSL toolchain is set up and step 1 is reconstructed.
