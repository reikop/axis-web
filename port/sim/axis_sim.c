/* AXIS per-tick simulation — reconstructed from the client's decompiled tick
 * functions (docs/SPEC.md §5). This is the per-tick update the network tick
 * `FUN_00497d70` drives after it applies inbound packets.
 *
 * ── Verification status (read this) ────────────────────────────────────────
 * These are FAITHFUL reconstructions transcribed from the decompiled functions
 * (exact field offsets and control flow preserved), NOT behaviour-verified.
 * AXIS is a client for a server-authoritative game and the `.ape` replays record
 * only *player* state (type-0 positions, type-14 HP), not projectile/effect
 * entity state — so there is no offline ground-truth to check the per-entity
 * physics against; that needs a live match, and the 2005 servers are gone. What
 * IS verified lives elsewhere and is used here: the shared RNG pool + the draw
 * consumption `axis_rng_draw` (247/247 on a real match, port/src/axis_rng.c) and
 * the type-0 state codec (6844/6844, port/src/axis_netdecode.c). The pieces here
 * are the structural/logic reconstruction; they compile, and they are honestly
 * marked unverified rather than presented as behaviour-exact.
 *
 * Memory model: the binary addresses its run-time state as `in_ECX + <abs off>`,
 * where `in_ECX` is the base of the ~30 MB game-state region. We mirror that
 * exactly: `S` is that base (`uint8_t*`), and every field is `S + <the exact
 * offset Ghidra shows>`. Entities are fixed-stride records inside `S`; actor
 * state blocks have stride 0x8888 at `S + 0x1ecca70 + actor*0x8888` (with the
 * fields the type-0 apply writes: pos +0x1ac/1b0/1b4, etc.). Callees that aren't
 * reconstructed yet are declared `extern` so the calls are preserved verbatim.
 */
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

/* the verified shared-pool draw (port/src/axis_rng.c) — the lockstep consumption */
uint32_t axis_rng_draw(const uint32_t *pool, uint32_t *idx, uint32_t n);

/* typed field accessors on the game-state base S (= the binary's in_ECX) */
static inline int32_t  *I(uint8_t *S, uint32_t o) { return (int32_t *)(S + o); }
static inline uint32_t *U(uint8_t *S, uint32_t o) { return (uint32_t *)(S + o); }
static inline float    *F(uint8_t *S, uint32_t o) { return (float *)(S + o); }

/* run-time callees not yet reconstructed (preserved verbatim from the decompile) */
extern void sim_entity_A(int i, float dt);        /* FUN_00475890 */
extern void sim_entity_proj(int i, float dt);     /* FUN_00479750 — projectile/weapon entity */
extern void sim_entity_C(int i, float dt);        /* FUN_0048f8d0 */

/* ── FUN_004962d0 — animation / effect frame-advance timer ──────────────────
 * Advances two frame counters at dt*0.025 and a 0..15 cycle at dt*15.0. (This
 * is also the §2.10 animation time base.) Faithful to the decompile. */
EXPORT
void sim_anim_tick(uint8_t *S, float dt) {
    if (*I(S, 0x2149308) == 0 || *I(S, 0x214930c) != 1) {
        float f = dt * 0.025f + *F(S, 0x9ef0a8);
        *F(S, 0x9ef0a8) = f;
        if (1.0f <= f) { *I(S, 0x9ef0a0) += 1; *F(S, 0x9ef0a8) = f - 1.0f; }
        f = dt * 0.025f + *F(S, 0x9ef0ac);
        *F(S, 0x9ef0ac) = f;
        if (1.0f <= f) { *I(S, 0x9ef0a4) += 1; *F(S, 0x9ef0ac) = f - 1.0f; }
        if (*I(S, 0x9ef30c) == 2) {
            f = dt * 15.0f + *F(S, 0x9ef900);
            *F(S, 0x9ef900) = f;
            if (1.0f <= f) {
                uint32_t v = (*U(S, 0x9ef8fc) + 1) & 0x8000000f;
                if ((int32_t)v < 0) v = ((v - 1) | 0xfffffff0) + 1;   /* mod 16, signed */
                *U(S, 0x9ef8fc) = v;
                *F(S, 0x9ef900) = f - 1.0f;
            }
        }
    }
}

/* ── FUN_0047c480 / FUN_00478f10 / FUN_004901d0 — per-entity update dispatchers ──
 * Each walks an active-flag array and calls a per-entity function. Strides and
 * array/count offsets are exact (SPEC §5 dispatcher map). */
EXPORT
void sim_dispatch_proj(uint8_t *S, float dt) {            /* FUN_0047c480 -> sim_entity_proj */
    if ((*I(S, 0x2149308) == 0 || *I(S, 0x214930c) != 1) && 0 < *I(S, 0x20a99b4)) {
        int n = *I(S, 0x20a99b4);
        for (int i = 0; i < n; i++)
            if (*I(S, 0x202ef38 + (uint32_t)i * 0x4e8) == 1) sim_entity_proj(i, dt);
    }
}
EXPORT
void sim_dispatch_A(uint8_t *S, float dt) {               /* FUN_00478f10 -> sim_entity_A */
    if ((*I(S, 0x2149308) == 0 || *I(S, 0x214930c) != 1) && 0 < *I(S, 0x20f2da0)) {
        int n = *I(S, 0x20f2da0);
        for (int i = 0; i < n; i++)
            if (*I(S, 0x20a99c4 + (uint32_t)i * 0x12c) == 1) sim_entity_A(i, dt);
    }
}
EXPORT
void sim_dispatch_C(uint8_t *S, float dt) {               /* FUN_004901d0 -> sim_entity_C */
    if ((*I(S, 0x2149308) == 0 || *I(S, 0x214930c) != 1) && 0 < *I(S, 0x13c9b24)) {
        int n = *I(S, 0x13c9b24);
        for (int i = 0; i < n; i++)
            if (*I(S, 0x131ae48 + (uint32_t)i * 0x2cc) == 1) sim_entity_C(i, dt);
    }
}
