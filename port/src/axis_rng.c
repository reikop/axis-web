/* AXIS shared RNG pool — the backbone of the game's lockstep determinism.
 *
 * At startup AXIS loads a 4096-entry random pool from MATHFUNC.TBL (AXIS.EXE
 * FUN_0046b6f0) and every peer consumes it by one shared index as
 * `pool[idx++ & 0xfff] % N`, so spawns / AI / damage rolls match across clients
 * (SPEC §5). The pool is just a precomputed table of standard MSVC `rand()`
 * outputs seeded with `srand(1)` — VERIFIED: regenerating it with the MSVC LCG
 * reproduces all 4096 entries of the shipped MATHFUNC.TBL exactly (4096/4096).
 *
 * So the determinism RNG is fully reconstructable without the data file:
 *   seed = seed*214013 + 2531011 ;  value = (seed >> 16) & 0x7fff
 *
 * This lets the WASM client build the identical shared pool and stay in lockstep
 * with the original game's decisions. Third reconstructed primitive of the port
 * (after axis_crypt.c and axis_netdecode.c).
 */
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

/* Fill `pool[0..4095]` with AXIS's shared RNG pool (MSVC rand() after srand(1)). */
EXPORT
void axis_rng_fill(uint32_t *pool) {
    uint32_t seed = 1;
    for (int i = 0; i < 4096; i++) {
        seed = seed * 214013u + 2531011u;
        pool[i] = (seed >> 16) & 0x7fff;
    }
}
