# axis-web

A browser port of **AXIS** (2005 Korean 3D action game), built by reverse-engineering
the original client and recompiling its logic to WebAssembly with a WebGL/WebAudio
platform shim.

> **Status:** reverse-engineering + asset pipeline complete — the game's maps, 3D models and
> textures all render in the browser. The interactive runtime (animation, logic, net, WASM)
> is the next phase.

## Quick start (local)

You need your own installed copy of AXIS. From the repo root:

```sh
python run.py                 # auto-detects ../Axis_Win, or:
python run.py /path/to/Axis_Win
```

This decodes your install (archives, 1,742 meshes, ~1,800 textures, 40 map layouts) and
opens a browser hub (`home.html`) linking four viewers:

- **3D Map Viewer** — walk the real maps, rendered from their actual meshes (WASD)
- **Mesh Viewer** — orbit any of the 1,742 extracted models
- **Texture Gallery** — every surface texture, decoded from the custom-JPEG codec
- **Archive Explorer** — browse the decrypted data archives

Nothing is uploaded and no game data enters the repo.

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
| 0. Analysis | RE engine, all file formats, GL usage → `docs/SPEC.md` | ✔ every asset format decoded |
| 1. Asset pipeline | Archive/PTX/mesh/map readers, catalog, decoders | ✔ archives, 1,742 meshes, textures, maps |
| 2. Renderer | Map + model + texture rendering in the browser | ✔ maps & models render (flat-shaded); textures standalone |
| 3. Game logic | Combat / items / physics | ◻ runtime phase |
| 4. Network | Protocol → Node server + WebSocket, online multiplayer | ◻ runtime phase |

What's left is the interactive runtime: texturing meshes (needs dynamic capture — see
`docs/SPEC.md §2.9`), skeletal animation, game logic, the network protocol & server, and the
Emscripten/WASM build (approach B, planned in `port/README.md`).

## Layout

```
run.py      one-shot: decode your install, serve web/, open the hub
web/        home hub + four offline, theme-aware viewers (map/mesh/texture/archive)
tools/      archive · obfuscation · PTX(JPEG) · .shp mesh · .bfd map decoders (Python)
docs/       reverse-engineering spec, formats, and procedure map
port/       approach-B WASM build plan
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
