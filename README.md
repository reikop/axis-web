# axis-web

A browser port of **AXIS** (2005 Korean 3D action game), built by reverse-engineering the
original OpenGL client and recompiling its logic to WebAssembly with a WebGL / WebAudio /
WebSocket platform shim (approach B — see `port/README.md`).

> **Status.** The entire offline-verifiable game runs in the browser as compiled WebAssembly:
> all eight asset/determinism decoders are reconstructed to C and verified **byte-exact** against
> the original data, and a compiled-C → WebGL **engine** renders real textured maps and meshes,
> is **walkable** (WASD + mouse-look), plays the game's music, and does **real-protocol
> multiplayer**. The movement model and the RNG lockstep determinism are reconstructed and
> **verified against recorded matches**. The port is **not yet complete**: the client's per-entity
> movement/physics tick (`FUN_00479750`, ~10 KB of vector math) is real code in `AXIS.EXE` and is
> **not yet reconstructed** — that is the honest remaining pillar (details below). Damage *scoring*
> is server-authoritative (the client only applies received HP).

## Quick start (local)

You need your own installed copy of AXIS. Nothing is uploaded and no game data enters the repo.

```sh
# 1) decode your install + serve the viewers and the engine
python run.py                 # auto-detects ../Axis_Win, or:
python run.py /path/to/Axis_Win
#    opens the hub (home.html); the "WASM Engine" card is the compiled port

# 2) build the compiled WASM/WebGL engine (once; needs Emscripten — see below)
cd port && ./build_engine.sh /path/to/Axis_Win
```

The hub links the viewers (map, mesh, textured mesh, skeleton, replay, textures, audio, archive,
the WASM decrypt/decode proofs) and the **WASM Engine** — a real Emscripten program that opens a
WebGL context and renders your maps/meshes from the same byte-exact reconstruction. Walk it with
WASD + Q/E/Space, look with the mouse (click to lock) or the arrow keys, play music, and connect
to the relay for multiplayer (`?map=NAME&mp`).

## ⚠️ You must own the game

This repository contains **only original code**. It ships **no game assets and no part of the
original executable** — those remain the copyright of the original rights holder. To run anything
here you must supply your own legally-owned copy of AXIS and point the tools at it. Nothing under
the game's data directory is ever committed (see `.gitignore`: `*.dat`, `*.bfd`, `*.ape`, `*.exe`,
`*.dll`, textures, audio, `*.wasm`, …).

## What works (all verified)

| Stage | Goal | State |
|-------|------|-------|
| 0. Analysis | RE the engine, every file format, GL/net usage → `docs/SPEC.md` | ✔ complete |
| 1. Decoders → WASM | archive · cipher · PTX(JPEG) · `.shp` mesh · `.bfd` map · skeleton · `.ape` replay · RNG | ✔ 8 modules, **byte-exact** vs references (`port/wasm/verify_emcc.js`) |
| 2. Renderer (compiled) | map + mesh + per-object textures in WebGL from compiled C | ✔ `port/engine/` → WebGL, screenshot-verified |
| 3. Platform shim | window/loop, keyboard, mouse, audio, networking | ✔ all pillars, 0 page-errors |
| 4. Network | §4 protocol → `server/relay.js` + WebSocket, real-protocol multiplayer | ✔ engine + `web/map.html`, verified vs a relay + ghost |
| 5. Movement + determinism | reconstruct + verify motion and RNG from recorded matches | ✔ `tools/verify_motion.js` (const-velocity) + `tools/verify_rng.js` (RNG lockstep **247/247**) |
| 6. Per-entity simulation | the per-tick movement/physics step (`FUN_00479750`) | ◻ **in the client, large, not yet reconstructed** (see below) |

### The genuinely unfinished pillar: the per-entity simulation

The port is **not** 100% complete, and I won't pretend otherwise. The client runs a substantial
per-entity simulation each tick: `FUN_0047c480` loops the active entities and calls **`FUN_00479750`**
— ~10 KB / 133 blocks of heavy floating-point (vector math, `sqrt`, ~400 float temporaries) over the
actor state block. That is real movement/physics code **in `AXIS.EXE`**, and it is **not yet
reconstructed**. (Two sibling functions earlier mislabelled as combat sim really are only helpers —
`FUN_004962d0` an animation frame-timer, `FUN_0048f1e0` a scene loader — but `FUN_00479750` is the
real thing; an earlier draft of these docs wrongly claimed the whole sim was server-side, and that is
retracted.)

What **is** done and verified: state application, the deterministic **RNG participation** (type-1
spawn/param draws, verified **247/247** against a real match), **movement kinematics** (verified
const-velocity), animation and rendering. Damage/hit *scoring* is server-authoritative (the client
only applies type-14 HP), but the local per-entity tick is client code a full port must reconstruct.

Reconstructing `FUN_00479750` (plus its callees and the full state-block field layout) is a large
multi-function effort; because the RNG stream is deterministic and verified, a reconstruction could be
checked by replaying a recorded match's inputs/RNG against the recorded positions. That work is
outstanding — left honestly unfinished rather than guess-filled, so nothing in this repo is presented
as verified that isn't.

## Build the engine (Emscripten)

Confirmed on Ubuntu/WSL2 with the distro toolchain:

```sh
sudo apt-get install -y --no-install-recommends emscripten build-essential cmake nodejs
cd port && ./build.sh /path/to/Axis_Win          # decoders → wasm/axis.wasm (+ verify, all pass)
         && ./build_engine.sh /path/to/Axis_Win  # the WebGL engine → web/engine/
```

`*.wasm` and the engine's generated `.js` glue are build artifacts (gitignored); regenerate with
the scripts above. `node port/wasm/build_wasm.js <game_dir>` also builds+verifies the core
primitives with no toolchain at all.

## Layout

```
run.py            one-shot: decode your install, serve web/ + /game/, open the hub
web/              home hub + viewers, and web/engine/ (the compiled WASM/WebGL engine page)
port/src/         reconstructed C: cipher, net codec, RNG, .shp/.bfd/PTX/skeleton/.ape decoders
port/engine/      the Emscripten engine (axis_engine.c) + build_engine.sh
port/wasm/        no-toolchain WASM build + verifiers (build_wasm.js, verify_emcc.js)
server/relay.js   zero-dependency WebSocket relay (the §4 packet model)
tools/            Python/JS decoders + verifiers (archive, cipher, PTX, mesh, map, skeleton, ape, motion)
docs/SPEC.md      the reverse-engineering spec: every format + the procedure map
```

## Legal

Reverse engineering for interoperability and writing an independent reimplementation is the model
used by projects like OpenMW and devilutionX. This project follows the same rule: **our code is
open; the game's assets are yours and stay out of the repo.** See `LICENSE` for the code license.
