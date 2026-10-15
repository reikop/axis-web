#!/usr/bin/env bash
# Build the AXIS engine harness (approach-B platform-shim slice) to WebAssembly
# with Emscripten's GL->WebGL layer. Unlike build.sh (a standalone decoder
# reactor), this is a full Emscripten program: main() opens a WebGL/GLES2 context
# and an emscripten main loop renders real AXIS mesh geometry decoded by the
# linked-in reconstruction (src/axis_shp.c). Game data is fetched by the page at
# run time (web/engine/index.html), never bundled.
#
#   cd axis-web/port && ./build_engine.sh
# Outputs web/engine/axis_engine.{js,wasm} (gitignored build artifacts).
set -euo pipefail
cd "$(dirname "$0")"

OUTDIR=../web/engine
mkdir -p "$OUTDIR"

if ! command -v emcc >/dev/null 2>&1; then
  echo "emcc not found (see build.sh header for the WSL setup)."; exit 1
fi

emcc engine/axis_engine.c src/axis_shp.c src/axis_ptx.c \
  -O2 -ffp-contract=off \
  -s FULL_ES2=1 -s MIN_WEBGL_VERSION=1 -s MAX_WEBGL_VERSION=1 \
  -s MODULARIZE=1 -s EXPORT_NAME=AxisEngine \
  -s EXPORTED_FUNCTIONS='["_main","_engine_alloc","_engine_free","_engine_load_mesh","_engine_load_map","_engine_set_texture","_engine_set_avatars","_engine_map_set_texture"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall","HEAPU8"]' \
  -s INITIAL_MEMORY=134217728 -s ALLOW_MEMORY_GROWTH=1 \
  -s ENVIRONMENT=web \
  -o "$OUTDIR/axis_engine.js"

echo "built $OUTDIR/axis_engine.js + .wasm"
ls -l "$OUTDIR"/axis_engine.*
