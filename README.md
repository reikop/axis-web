# axis-web

A browser port of **AXIS** (2005 Korean 3D action game), built by reverse-engineering
the original client and recompiling its logic to WebAssembly with a WebGL/WebAudio
platform shim.

> **Status:** early — reverse-engineering the file formats and engine (stage 0).

## ⚠️ You must own the game

This repository contains **only original code**. It ships **no game assets and no part
of the original executable** — those remain the copyright of the original rights holder.
To run anything here you must supply your own legally-owned copy of AXIS and point the
tools at it. Nothing under the game's data directory is ever committed (see `.gitignore`).

## Approach

Rendering in the original is **OpenGL** (not Direct3D), which makes an Emscripten →
WebGL port realistic. Plan:

| Stage | Goal |
|-------|------|
| 0. Analysis | RE the engine, file formats, GL usage, net protocol → `docs/SPEC.md` |
| 1. Asset pipeline | Decoders for `Shape.dat`/`Image*.dat`/`Map/*.bfd` → glTF/PNG, + web viewer |
| 2. Renderer | Map + model + animation rendering on WebGL |
| 3. Game logic | Combat / items / physics (WASM, behaviour-preserving) |
| 4. Network | Reverse the protocol → Node server + WebSocket, online multiplayer |

## Layout

```
tools/      asset extractors & analysis scripts (Python)
docs/       reverse-engineering spec and notes
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
