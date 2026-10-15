# Approach B — WASM port plan

## Status: the WASM path is live and verified — the whole data layer

The Emscripten toolchain is set up and working (Ubuntu/WSL2, `emcc 3.1.69`, `node v22`), and the
**entire offline-verifiable AXIS data + determinism layer is now reconstructed to C, compiled to
one standalone WASM reactor (`wasm/axis.wasm`, ~13 KB), and verified byte-exact against the
JS/Python references on the user's real game files** by `wasm/verify_emcc.js`:

| `src/*.c` | what it reconstructs | verified on real data |
|-----------|----------------------|------------------------|
| `axis_crypt.c`     | obfuscation cipher (`FUN_0046c060/c0b0`, §2.3) | 10/10 `Map/*.bfd` + `*.ape` + `Sound.dat` byte-identical |
| `axis_netdecode.c` | type-0 state-packet codec, decode+encode (`FUN_0048bca0`/`d750`, §4) | decode 6844/6844, encode 6844/6844 f32-exact |
| `axis_rng.c`       | shared deterministic RNG pool (`MATHFUNC.TBL`, §5) | 4096/4096 vs MSVC `rand(srand(1))` and the shipped pool |
| `axis_shp.c`       | `.shp` mesh geometry (§2.1/§2.8) | 1743/1743 meshes byte-identical |
| `axis_bfd.c`       | `.bfd` map decrypt + object placements (§2.6) | 40/40 maps byte-identical |
| `axis_ptx.c`       | PTX baseline-JPEG texture decode (§2.2) | 87/87 sampled textures byte-identical |
| `axis_skel.c`      | `Skeleton.dat` bone records (§2.10) | 232/232 rigs byte-identical |
| `axis_ape.c`       | `.ape` replay trajectories (§4) | 4/4 replays byte-identical |

So a WASM client parses and emits the game's real wire format, reproduces its exact lockstep RNG,
and decodes every mesh, map, texture, rig and replay straight from the user's raw files — all
compiler-built, no JS fallback needed. Build + verify: `./build.sh /path/to/Axis_Win`.

The cipher/state/rng core is **also** verified a second, independent way for environments without
emcc: `node wasm/build_wasm.js <game_dir>` hand-assembles a byte-identical module with no
toolchain (same checks pass), emits `web/crypt.js` (module embedded for the browser), and
`web/wasm.html` shows it running in-browser with the **WASM** backend (all PASS, 0 errors).

`wasm/axis.wasm` is a build artifact and is gitignored (regenerate with `build.sh`).

### Engine harness — the platform shim has started (compiled C → WASM → WebGL)

Beyond the decoder reactor, `engine/axis_engine.c` is a **real Emscripten program**: `main()` opens
a WebGL/GLES2 context through Emscripten's GL→WebGL layer and an emscripten main loop renders actual
AXIS geometry decoded by the linked-in, byte-exact `axis_shp.c`. Build it with `./build_engine.sh`
(emcc `FULL_ES2`, ~22 KB wasm + ~42 KB glue, both gitignored); it serves at `web/engine/index.html`
(linked from the home hub), fetching the user's own files — nothing bundled. Verified in a browser
(headless Chrome, SwiftShader WebGL):

- `?i=N` decodes and renders one Shape.dat mesh;
- `?map=CUBE` decrypts a `Map/*.bfd`, matches each placed object's name to a Shape.dat record the
  way `web/map.html` does, decodes the mesh, positions it, and renders the whole arena (68 objects
  for CUBE) — screenshot-verified, no page errors;
- a first-person camera makes it walkable: **WASD move + Q/E/Space up-down** (movement verified
  end-to-end — a real held keydown drives the camera into the arena, screenshot-confirmed), arrow
  / I-J-K-L look, and **pointer-lock mouse look** (click the canvas);
- **textures:** in mesh mode (`?i=N`) the engine finds the shape's `SHAPE\<name>` skin across the
  Image archives, decodes it with the linked-in `axis_ptx.c`, and renders it UV-mapped — verified
  (`?i=5` = `ALIEN\GIRL` textured with its real `SHAPE\ALIEN\GIRL` 256×256 skin);
- **audio:** a ♪ button streams a `Music/*.mp3` track from the user's install (WebAudio slice);
- **networking:** with `?map=NAME&mp` the engine connects to the AXIS relay (`server/relay.js`),
  decodes each remote player's 64-byte payload with the **real type-0 wire codec** (`crypt.js`
  `decodeState`/`encodeState`, backend `wasm`) and renders them as avatars — verified end-to-end
  against the live relay + a Node ghost: the engine joins, decodes the ghost's real type-0 packets
  and draws it as a floating avatar cube, status *"1 remote player via ws://… (real type-0 codec,
  backend=wasm)"*, 0 page-errors.

So the reconstruction now runs as compiled code driving real GPU rendering with textures, input,
audio and real-protocol multiplayer — not only as decoders. The shim pieces (GL, window/loop,
keyboard, mouse, audio, textures, networking) are in and verified clean (0 page-errors).

**What's left for a *playable* port:**
- **per-object map textures** (most building skins aren't statically resolvable — §2.9 — so this
  is partial by nature and needs per-object draws);
- the behaviour-exact **per-tick simulation** — the one genuinely blocked pillar: online play in
  the engine today is shared positions (like the game's state replication) but *not* the original
  deterministic combat/physics. That sim runs only inside a live match, and the dead-server install
  can't reach one, so there is no ground-truth to reconstruct and verify against here (see
  `../docs/SPEC.md` §5 and §2.9). The RNG/determinism core and the sim entry points are mapped and
  reconstructed; the behaviour itself is the blocked piece.

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
