# axis-web

A browser port of **AXIS** (2005 Korean 3D action game), built by reverse-engineering the
original OpenGL client and recompiling its logic to WebAssembly with a WebGL / WebAudio /
WebSocket platform shim (approach B — see `port/README.md`).

> **Status.** The entire offline-verifiable game runs in the browser as compiled WebAssembly:
> all eight asset/determinism decoders are reconstructed to C and verified **byte-exact** against
> the original data, and a compiled-C → WebGL **engine** renders real textured maps and meshes,
> is **walkable** (WASD + mouse-look), plays the game's music, and does **real-protocol
> multiplayer**. The movement model and the RNG lockstep determinism are reconstructed and
> **verified against recorded matches**. AXIS is a client for a server-authoritative game, so the
> combat resolution (collision/hit-detection/damage) was **server-side code, never part of the
> shipped client** — the client itself is fully reconstructed; that defunct server is the only
> thing absent, and it isn't in `AXIS.EXE` to port (details below).

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
| 6. Authoritative combat | collision / weapon hit-detection / damage | server-side — **not in the client** (see below) |

### Why "combat simulation" isn't a missing client module

AXIS is a client for a server-authoritative online game. The client **sends** input and **receives**
its combat results as state — positions (type-0) and **HP (type-14)** — and *applies* them; it does
not compute collision/hit-detection/damage. Decoding the functions once labelled the "per-tick combat
sim" confirms this: they turn out to be client per-frame helpers — `FUN_004962d0` is an animation
frame-advance timer, `FUN_0048f1e0` a scene loader — not physics (see `docs/SPEC.md §5`). So the
authoritative combat resolution lived in the **AxisArena server**, a separate program that is not part
of the shipped `AXIS.EXE` and no longer exists (the 2005 servers are gone; the client can't even reach
a match — it stalls at "INITIALIZING NETSPHERE").

What the **client** actually does is fully reconstructed and, where recorded matches give ground-truth,
verified: state application, the deterministic **RNG participation** (type-1 spawn/param draws, verified
247/247 against a real match), **movement** kinematics (verified), animation, and rendering. There is
no client-side combat-physics code to port; inventing the server's logic with no ground-truth would be
a guess, which this project does not ship. So the **client port is complete** — the "missing" piece is
a defunct server, not an unfinished part of the client.

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
