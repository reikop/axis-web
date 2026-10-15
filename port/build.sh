#!/usr/bin/env bash
# Build the reconstructed AXIS native code to WebAssembly with Emscripten.
#
# This is the approach-B toolchain path (run it in WSL or any box with emsdk):
#   1. git clone https://github.com/emscripten-core/emsdk && cd emsdk
#      ./emsdk install latest && ./emsdk activate latest && source ./emsdk_env.sh
#   2. cd axis-web/port && ./build.sh
#
# It compiles the reconstructed C in src/ to a standalone WASM module exporting
# the AXIS primitives, for the browser client to load. As more of the game is
# reconstructed (archive index, PTX codec, .shp mesh, map decode, then the
# simulation) each lands as another src/*.c and is added to SRC below.
#
# NOTE: the environment that produced this repo had no emcc, so the cipher
# module is *also* hand-assembled + verified by `node wasm/build_wasm.js`
# (identical semantics, no toolchain needed). This script is the full path that
# supersedes it once emsdk is present.
set -euo pipefail
cd "$(dirname "$0")"

SRC=(src/axis_crypt.c)
OUT=wasm/axis.wasm

EXPORTS='["_axis_decrypt","_axis_encrypt","_malloc","_free"]'

if ! command -v emcc >/dev/null 2>&1; then
  echo "emcc not found. Install emsdk first (see the header of this script)."
  echo "Meanwhile, 'node wasm/build_wasm.js <game_dir>' builds + verifies the"
  echo "cipher module with no toolchain."
  exit 1
fi

emcc "${SRC[@]}" \
  -O3 -flto \
  -s STANDALONE_WASM=0 \
  -s MODULARIZE=1 -s EXPORT_NAME=AXISModule \
  -s EXPORTED_FUNCTIONS="$EXPORTS" \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPU8"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -o "$OUT"

echo "built $OUT"
ls -l "$OUT"
