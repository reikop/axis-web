#!/usr/bin/env bash
# Build the reconstructed AXIS native code to WebAssembly with Emscripten.
#
# This is the approach-B toolchain path. VERIFIED WORKING on Ubuntu/WSL2 with
# the distro emscripten (apt): emcc 3.1.69, node v22. Setup that was used:
#   sudo apt-get install -y --no-install-recommends \
#        emscripten build-essential cmake nodejs
#   emcc --version          # 3.1.69
# (or the upstream emsdk: git clone emscripten-core/emsdk && ./emsdk install latest
#  && ./emsdk activate latest && source ./emsdk_env.sh)
# Then:  cd axis-web/port && ./build.sh [<game_dir>]
#
# It compiles the reconstructed C in src/ to ONE standalone WASM reactor module
# (no JS runtime, no imports) exporting the AXIS primitives + memory, so the
# browser client -- and wasm/verify_emcc.js -- can instantiate it directly, the
# same shape as the hand-assembled module in wasm/build_wasm.js. As more of the
# game is reconstructed (archive index, PTX codec, .shp mesh, map decode, then
# the simulation) each lands as another src/*.c added to SRC below; the larger
# pieces can only be built this way (not hand-assembled).
#
# The cipher/state/rng module is *also* hand-assembled + verified with no
# toolchain by `node wasm/build_wasm.js` (identical semantics); this script is
# the full compiler path, and wasm/verify_emcc.js checks its output against the
# same real game data (cipher 10/10, state 6844/6844, rng 4096/4096).
set -euo pipefail
cd "$(dirname "$0")"
GAME="${1:-../../Axis_Win}"

SRC=(src/axis_crypt.c src/axis_netdecode.c src/axis_rng.c src/axis_shp.c src/axis_bfd.c src/axis_ptx.c src/axis_skel.c src/axis_ape.c)
OUT=wasm/axis.wasm

EXPORTS='["_axis_decrypt","_axis_encrypt","_axis_decode_state","_axis_encode_state","_axis_rng_fill","_axis_rng_draw","_axis_shp_parse","_axis_bfd_extract","_axis_ptx_decode","_axis_skel_parse","_axis_ape_parse"]'

if ! command -v emcc >/dev/null 2>&1; then
  echo "emcc not found. Install it first (see the header of this script)."
  echo "Meanwhile, 'node wasm/build_wasm.js <game_dir>' builds + verifies the"
  echo "cipher/state/rng module with no toolchain."
  exit 1
fi

# Standalone reactor: --no-entry + STANDALONE_WASM, functions kept via
# EMSCRIPTEN_KEEPALIVE in the sources. 48 MiB initial (grows to 256 MiB) so any
# single game file fits above the 16 MiB scratch floor the verifier uses.
emcc "${SRC[@]}" \
  -O3 --no-entry -ffp-contract=off \
  -s STANDALONE_WASM=1 \
  -s INITIAL_MEMORY=50331648 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s MAXIMUM_MEMORY=268435456 \
  -s EXPORTED_FUNCTIONS="$EXPORTS" \
  -s ERROR_ON_UNDEFINED_SYMBOLS=1 \
  -o "$OUT"

echo "built $OUT ($(wc -c < "$OUT") bytes)"

if command -v node >/dev/null 2>&1; then
  echo "--- verifying against real game data ($GAME) ---"
  node wasm/verify_emcc.js "$OUT" "$GAME"
else
  echo "node not found -- skipping verification"
fi
