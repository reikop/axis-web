# axis-web

A browser port of **AXIS** (2005 Korean 3D action game), built by reverse-engineering the
original OpenGL client and recompiling its logic to WebAssembly with a WebGL / WebAudio /
WebSocket platform shim (approach B — see `port/README.md`).

> **Status.** The entire offline-verifiable game runs in the browser as compiled WebAssembly.
> All eight asset/determinism decoders are reconstructed to C and verified **byte-exact** against
> the original data; a compiled-C → WebGL **engine** renders real textured maps and meshes, is
> **walkable** (WASD + mouse-look, with **collision against the real map geometry** — you can't
> walk through buildings), plays the game's music, and does **real-protocol multiplayer**. The
> **entire client per-tick simulation surface is reconstructed as clean, independently-verified
> layers** (`port/sim/`) and **wired live into the engine**: homing-projectile combat, broad- +
> narrow-phase collision, gravity debris/effects, animated proximity props, item pickups, and
> edge-triggered input. Together they make a playable loop on the real maps — walk an AXIS arena
> and press **F** to fire a projectile that homes, collides with the level, and bursts debris
> (`?map=NAME&proj`). Movement kinematics and RNG lockstep determinism are verified **against
> recorded matches** (RNG **247/247**).
>
> **The one genuinely unreconstructed piece** is the authoritative **input → acceleration**
> transfer. Reading the client's per-tick code shows the player's position/orientation are *read
> from* the networked actor state (type-0 snapshots + client dead-reckoning), never computed from
> input on the client — so that transfer runs server-side, and with the original servers long gone
> there is no offline ground truth to reconstruct or verify it against. Interactive walking uses the
> **replay-verified** movement model instead (const-velocity dead-reckoning, SPEC §5), so this gap is
> a feel nuance, not a hole in a playable port. Everything the client itself runs per tick **is**
> reconstructed and verified; details below.
>
> **Verified end-to-end in a browser (2026-10-16).** Served locally and driven headless: the WebGL
> engine renders real maps (e.g. Metropolis, 118 objects), the reconstructed simulation runs live
> (`?map=NAME&proj` — projectiles, gravity debris, pickups, destruction counter), and the **3D replay**
> plays a real recorded 2005 match (`Autoplay001.ape`: players *k'*, *OneShotOneKill*, *onlynike* over
> Metropolis) — all with **zero page errors**. The original client has the same offline playback built
> in (its attract-mode `processAutoPlayMode`, reverse-engineered in `docs/SERVER.md`): forcing it past
> its launcher-gated boot shows it loading the very same `AutoPlay/*.ape` into its real engine and
> running the in-match pipeline (actors driven, per-tick + render executing) with **no server** — this
> web port does that same thing, and renders it visibly, which the gated native client cannot offline.
> Model posing in the reachable render path is **orientation-from-actor-state** (8-direction billboard
> facing + pitch/yaw in tenths-of-a-degree via the engine's sin/cos LUTs), not time-keyframe
> interpolation — i.e. exactly what the replay reproduces.

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
WASD + Q/E/Space (collision stops you at walls), look with the mouse (click to lock) or the arrow
keys, play music, connect to the relay for multiplayer (`?map=NAME&mp`), and run the reconstructed
simulation with `?map=NAME&proj` — **press F to fire**.

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
| 6. Per-tick simulation | reconstruct every client per-tick system as verified layers + wire them live | ✔ `port/sim/` — all four dispatchers + collision + input, native-test verified, running in the engine |

### The boundary — what isn't included, and why (measured)

Two client pieces are **not** reconstructed from a *live* match: the authoritative **input →
acceleration** transfer, and the skeleton **animation keyframe sampler** (the per-key stride/time-base
isn't in the file). Both run only in a live match, and the 2005 servers are gone — this is **measured,
not assumed**. But the in-match *data* is **not** out of reach offline: AXIS has a **built-in,
server-free, in-engine AutoPlay replay path**, and running `AXIS.EXE` under Frida (2026-10-16) shows
its **logic runs end-to-end offline** — forcing the scene-state sentinel to "ready" and pointing its
loader at `AutoPlay/Autoplay001.ape` makes `processAutoPlayMode` open the file (version `0x1201`, map
13) and run the full load pipeline (ground → objects → collision → battlefield → actors) into the real
engine, then enter playback mode and hold. The one native-only piece left for on-screen pixels in
`AXIS.EXE` is the boot-splash → game-renderer presentation handoff (a separate subsystem tied to the
absent launcher). **The web port doesn't need it** — it decodes and renders the same `.ape` replays
itself (`web/ape.js` + the engine replay mode), so it already does what the native AutoPlay path does,
visibly. What still needs a live server is only **interactive** play's input→acceleration transfer
(replay reads positions from the file). See `docs/SERVER.md` (headline section) for the full mapping —
the state machine, the game-object base pointer, `processAutoPlayMode`, and the live Frida trace — and
`docs/SPEC.md` §2.10 / §5 for the simulation/animation measurements.

### The per-tick simulation (reconstructed, verified, wired)

The client runs four per-entity dispatchers each tick plus a shared collision query; **all of them,
and the client input path, are reconstructed as clean original C in `port/sim/`**, each with a
native-`cc` test that checks its behaviour against mathematical/geometric identities (no live server
needed), and all are **wired into the engine** and exercised in the browser on real maps:

| Client per-tick system | origin | reconstruction |
|------|--------|----------------|
| projectile combat (homing, swept hit, knockback) | `FUN_00479750` / `FUN_0047c480` | `axis_proj.c` + `axis_tick.c` + `axis_target.c` + `axis_vecmath.c` |
| collision broad phase (segment → local AABB, Cohen–Sutherland) | `FUN_00470bc0` | `axis_broadphase.c` |
| collision narrow phase (segment ↔ triangle) | `FUN_004824e0` | `axis_collide.c` |
| effects / gravity debris | `FUN_00475890` (type A) | `axis_effect.c` |
| animated / interactive props (phase, proximity, glow) | `FUN_0048f8d0` (type C) | `axis_prop.c` |
| item / ammo pickups | `FUN_0048e220` | `axis_pickup.c` |
| client input (edge-triggered action masks) | `FUN_0048d080` sampling half | `axis_input.c` |

The `?proj` demo runs these together on any loaded map: homing projectiles collide with the real
building geometry and burst debris, a proximity gate opens, pickups are collected, and **F fires** a
camera projectile — a minimal playable shoot loop built entirely from the reconstructed layers,
verified in-browser on CUBE **and** real maps (e.g. Metropolis, 118 objects), 0 page-errors.

Two honest caveats. (1) The per-entity physics uses x87 80-bit (`float10`) intermediates in the
original; WebAssembly has only f32/f64, so a reconstruction is **internally consistent (web-only)**,
not bit-identical to 2005 clients — which no longer exist to play against anyway. (2) The one piece
**not** reconstructed is the authoritative **input → acceleration** transfer: the client reads its
position from networked state (type-0) rather than computing it from input, so that lives server-side
with no offline ground truth. Damage/hit *scoring* is likewise server-authoritative (the client only
applies type-14 HP). Nothing here is presented as verified that isn't.

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
port/sim/         reconstructed per-tick simulation layers (+ native-cc tests): projectiles,
                  collision broad/narrow phase, effects/debris, props, pickups, input
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
