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
- `src/axis_rng.c` — `axis_rng_fill` builds the game's **shared deterministic RNG pool** (the
  lockstep core, SPEC §5). AXIS loads a 4096-entry pool from `MATHFUNC.TBL`; it is exactly the
  MSVC `rand()` LCG seeded with `srand(1)`. **Verified: the WASM fill matches the MSVC reference
  4096/4096 AND the shipped MATHFUNC.TBL pool 4096/4096**, so a WASM client reproduces the exact
  shared pool and can stay in lockstep with the original game's spawns/AI/damage decisions.

Both are verified **two independent ways**, and both now pass on the real game data:

- **Emscripten compiler path (`build.sh`)** — now confirmed working. Built with the Ubuntu/WSL2
  distro emscripten (`emcc 3.1.69`, `node v22`) to a 1102-byte standalone reactor WASM, then
  `wasm/verify_emcc.js` instantiates it headless and checks it against your real files:
  cipher **10/10** byte-identical, state decode **6844/6844** and encode **6844/6844** f32-exact
  (plus encode∘decode recovering every original packet), rng **4096/4096** vs the MSVC reference
  **and** the shipped `MATHFUNC.TBL` pool. This is the path the larger reconstructions (mesh/map
  decode, then the simulation) must use — they can't be hand-assembled.
- **No-toolchain path (`node wasm/build_wasm.js <game_dir>`)** — for environments without emcc,
  **hand-assembles the identical module** (~300 bytes) and runs the same checks (cipher 10/10,
  state 6844/6844, rng 4096/4096). It also emits `web/crypt.js` (the module embedded for the
  browser), and `web/wasm.html` shows it decrypting your files in-browser with the **WASM**
  backend (verified: all PASS, 0 errors).

So approach B is proven end-to-end **both** by a real compiler and by the no-toolchain fallback:
reconstructed AXIS logic → WASM → runs in Node and in the browser on real data, bit-for-bit
matched between the two builds and the reference. Each further `src/*.c` (archive index, PTX
codec, `.shp` mesh, map decode, then the simulation) is added to `build.sh`'s `SRC` list the
same way; `wasm/axis.wasm` is a build artifact and is gitignored (regenerate with `build.sh`).

---

Reconstruct the client as C++ and compile it to WebAssembly with Emscripten, replacing
the Win32 / OpenGL / DirectSound / DirectInput / Winsock platform layer with a web shim.
This keeps the game's own logic and data formats behaviour-exact; only the OS boundary is
rewritten. Rendering is OpenGL, so Emscripten's GL→WebGL layer does most of the graphics
work. See `../docs/SPEC.md` for formats and the procedure map.

## Toolchain (WSL) — set up and confirmed working

```sh
# in WSL (Ubuntu 2); the distro emscripten is enough and is what this repo used
sudo apt-get update
sudo apt-get install -y --no-install-recommends emscripten build-essential cmake nodejs
emcc --version               # 3.1.69 confirmed
# then, from axis-web/port:
./build.sh /path/to/Axis_Win # compiles src/*.c -> wasm/axis.wasm and verifies it

# (alternatively the upstream emsdk for a newer emcc:)
#   git clone https://github.com/emscripten-core/emsdk
#   cd emsdk && ./emsdk install latest && ./emsdk activate latest && source ./emsdk_env.sh
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

The WSL/Emscripten toolchain is set up and **the WASM build path is live and verified**
(see the top of this file): the cipher, the type-0 state codec, and the shared RNG pool are
reconstructed to C, compiled with `emcc` to a standalone WASM module, and checked byte/f32-exact
against the real game data. The platform shim (GL/window/input/audio/sockets, table above) and
the larger reconstructions (PTX codec, `.shp` mesh, `.bfd` map, then the per-tick simulation)
are the remaining work; each is added as another `src/*.c` and verified the same way. Formats
and the procedure map are in `../docs/SPEC.md`.
