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

/* One shared-pool draw, advancing the shared index: `pool[idx++ & 0xfff] % n`
 * (n == 0 returns the raw value). This is the exact consumption the game uses
 * everywhere its lockstep decisions are made — `pool[DAT_00a20488++ & 0xfff] % N`
 * in the per-tick sim and the packet builders (e.g. FUN_0048d080). It is the
 * operational core of the determinism: two peers starting at the same index and
 * drawing in the same order get identical spawns / rolls.
 *
 * VERIFIED against real recorded play: a type-1 packet's RNG fields are two
 * successive `axis_rng_draw(pool,&idx,100000)` values of this pool — matched
 * 247/247 on Test.ape by tools/verify_rng.js, and re-checked here in
 * port/wasm/verify_emcc.js by reproducing those packets' [44]/[50] from the WASM
 * draw at each packet's recovered index. */
EXPORT
uint32_t axis_rng_draw(const uint32_t *pool, uint32_t *idx, uint32_t n) {
    uint32_t v = pool[(*idx)++ & 0xfffu];
    return n ? (v % n) : v;
}
