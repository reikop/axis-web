# axis-web

A browser port of **AXIS** (2005 Korean 3D action game), built by reverse-engineering
the original client and recompiling its logic to WebAssembly with a WebGL/WebAudio
platform shim.

> **Status:** stage 0 (reverse-engineering) well underway; a local archive explorer runs today.

## Quick start (local)

You need your own installed copy of AXIS. From the repo root:

```sh
python run.py                 # auto-detects ../Axis_Win, or:
python run.py /path/to/Axis_Win
```

This builds a metadata catalog from your install and opens a browser explorer of every
archive, texture (PTX) and map. Nothing is uploaded and no game data enters the repo.

## ⚠️ You must own the game

This repository contains **only original code**. It ships **no game assets and no part
of the original executable** — those remain the copyright of the original rights holder.
To run anything here you must supply your own legally-owned copy of AXIS and point the
tools at it. Nothing under the game's data directory is ever committed (see `.gitignore`).

## Approach

Rendering in the original is **OpenGL** (not Direct3D), which makes an Emscripten →
WebGL port realistic. Plan:

| Stage | Goal | State |
|-------|------|-------|
| 0. Analysis | RE the engine, formats, GL usage, net protocol → `docs/SPEC.md` | ▶ archive + PTX + map pipeline done; terrain/mesh/net next |
| 1. Asset pipeline | Archive + PTX readers, catalog, local web explorer | ✔ explorer runs; PTX→PNG pending |
| 2. Renderer | Map + model + animation on WebGL (via the WASM build) | ◻ |
| 3. Game logic | Combat / items / physics (WASM, behaviour-preserving) | ◻ |
| 4. Network | Reverse the protocol → Node server + WebSocket, online multiplayer | ◻ |

The port itself (approach B — reconstruct C++ → Emscripten/WASM + web shim) is planned in
`port/README.md`.

## Layout

```
run.py      one-shot: build catalog from your install, serve web/, open browser
web/        offline, theme-aware archive explorer (loads a generated catalog.json)
tools/      archive/PTX parsers, catalog builder, extractor (Python)
docs/       reverse-engineering spec and procedure map
port/        approach-B WASM build plan and (later) reconstructed C++ + shim
```

## Tools

```sh
# Parse an indexed archive (point at YOUR game's copy)
python -I tools/parse_archive.py /path/to/AXIS/Data/Shape.dat
```

## Legal

Reverse engineering for interoperability and writing an independent reimplementation
is the model used by projects like OpenMW and devilutionX. This project follows the
same rule: **our code is open; the game's assets are yours and stay out of the repo.**
