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

/* ── FUN_0048f1e0 — scene update/load helper ────────────────────────────────
 * When the scene is active, tick its loader and, on a pending path, (re)load it.
 * Faithful to the decompile; string args preserved as the scene name/path. */
extern void FUN_0048a8a0(void *scene, const char *name);
extern int  FUN_0048a8b0(int arg);
extern void FUN_0049e03d(char *out, const char *fmt, void *arg);  /* sprintf-like */
extern void FUN_00456510(char *path, int n);                      /* scene load */
EXPORT
void sim_scene_tick(uint8_t *S, int arg) {
    if (*I(S, 0x2149308) != 0) {
        if (*I(S, 0x2148ee0) == 0) FUN_0048a8a0(S + 0x2149208, "Scene");
        if (FUN_0048a8b0(arg) == 0) *I(S, 0x2149308) = 0;
        if (*(char *)(S + 0x2149108) != '\0') {
            char path[256];
            FUN_0049e03d(path, "scenePath=%s", S + 0x2149108);
            FUN_00456510(path, 100);
        }
    }
}

/* ── FUN_0048d080 — player INPUT/action packet builder (type 1), SPEC §4 ─────
 * Builds and sends the 64-byte type-1 packet for `actor`: appearance, aim/input
 * floats, per-channel action/fire bitmasks (the 0x10 loop), the stream selector
 * and TWO shared-pool draws (+ spawn/param table lookups), then sends + encrypts.
 * The pool draws use the VERIFIED axis_rng_draw (pool @ S+0xa11bc8, idx @
 * S+0xa20488; packet[44]/[50] = draw%100000, matched 247/247 on a real match).
 * The per-stream spawn/param tables (S+0x15bd1f8 …) are run-time data; the index
 * math is reconstructed exactly, the looked-up values come from that live data. */
extern void     FUN_004a79c0(int len, void *buf);                 /* pnSendPacketData */
extern int      FUN_004a7d60(int channel);                        /* channel-active? */
extern void     FUN_0046c0b0(void *p, int sz, int cnt, uint32_t h);/* encrypting write */
EXPORT
void sim_build_input(uint8_t *S, int actor) {
    uint8_t buf[64];
    if (*I(S, 0x2343006 + actor * 0x10) == 0) return;

    buf[1] = 1;                                   /* type 1 */
    buf[2] = (uint8_t)actor;
    if (*I(S, 0x1f1ca8) < 1 || 0.0f < *F(S, 0x234239d)) {
        buf[3] = buf[4] = buf[5] = buf[6] = 0xff;      /* inactive: appearance all 0xff */
        *(uint16_t *)(buf + 7) = 0xffff;
    } else {
        int a = *I(S, 0x1f1cac) * 0x10;
        buf[3] = *(uint8_t *)(S + 0x1f1668 + a);
        buf[4] = *(uint8_t *)(S + 0x1f1cb4);
        buf[5] = *(uint8_t *)(S + 0x1f166c + a);
        buf[6] = *(uint8_t *)(S + 0x1f1cb8);
        *(uint16_t *)(buf + 7) = *(uint16_t *)(S + 0xbe3000);
    }
    *(uint32_t *)(buf + 9)  = *U(S, 0x22f0408);        /* aim A */
    *(uint32_t *)(buf + 13) = *U(S, 0x22f040c);        /* aim B */

    uint32_t *pool = U(S, 0xa11bc8), *idx = U(S, 0xa20488);
    uint8_t sel = (uint8_t)(*U(S, 0x23423c9) % 5);
    *U(S, 0x23423c9) += 1;
    buf[43] = sel;
    /* per-stream spawn/param tables + divisors/counts (exact offsets from the decompile) */
    static const uint32_t TBL[5]   = {0x15bd1f8, 0x15de6cc, 0x15fff88, 0x163b58c, 0x1679b38};
    static const uint32_t DIVO[5]  = {0x23423e5, 0x23423e9, 0x23423ed, 0x23423f1, 0x23423f5};
    static const uint32_t CNTO[5]  = {0x15de6c8, 0x15fff84, 0x163b588, 0x1679b34, 0x16c5178};
    static const uint32_t STR[5]   = {0x2aa, 0x2af, 0x4c0, 0x4fd, 0x608};
    uint32_t d1 = axis_rng_draw(pool, idx, 100000);    /* draw 1 */
    *(uint16_t *)(buf + 44) = (uint16_t)d1;
    {   uint32_t v = d1 & 0xffff, div = *U(S, DIVO[sel]), cnt = *U(S, CNTO[sel]);
        uint32_t col = v % div, row = (v / div) % cnt;
        *(uint32_t *)(buf + 46) = *U(S, TBL[sel] + (col + row * STR[sel]) * 4);
    }
    uint32_t d2 = axis_rng_draw(pool, idx, 100000);    /* draw 2 (the final/common draw) */
    *(uint16_t *)(buf + 50) = (uint16_t)d2;
    {   uint32_t v = d2 & 0xffff, div = *U(S, 0x23423f9), cnt = *U(S, 0x175522c);
        uint32_t col = v % div, row = (v / div) % cnt;
        *(uint32_t *)(buf + 52) = *U(S, 0x16c517c + (col + row * 0xb86) * 4);
    }

    uint32_t m1 = 0, m2 = 0;                            /* action/fire bitmasks */
    for (int ch = 0; ch < 0x10; ch++) {
        if (FUN_004a7d60(ch) == 1) {
            int32_t *q = I(S, 0x2343012 + (uint32_t)ch * 0x10);
            if (q[-1] == 1) { q[-1] = 0; m1 |= 1u << ch; }
            if (q[0]  == 1) { q[0]  = 0; m2 |= 1u << ch; }
        }
    }
    *(uint32_t *)(buf + 17) = *U(S, 0x1eccb6c + actor * 0x8888);   /* input A */
    *(uint32_t *)(buf + 21) = *U(S, 0x1eccb70 + actor * 0x8888);   /* input B */
    *(uint32_t *)(buf + 25) = m1;
    *(uint32_t *)(buf + 29) = m2;
    *(uint16_t *)(buf + 33) = *(uint16_t *)(S + 0x1eccb54 + actor * 0x8888);
    *(uint16_t *)(buf + 35) = *(uint16_t *)(S + 0x2221fb8);
    *(uint16_t *)(buf + 37) = *(uint16_t *)(S + 0x1eccb58 + actor * 0x8888);
    *(uint16_t *)(buf + 39) = *(uint16_t *)(S + 0x2221fbc);
    *(uint16_t *)(buf + 41) = *(uint16_t *)(S + 0x22f0404);
    *U(S, 0x22f0404) = 0xffffffff;
    *I(S, 0x2343006 + actor * 0x10) = 0;

    FUN_004a79c0(0x40, buf);                           /* send */
    *I(S, 0x2340c78) += 1;
    if (*I(S, 0x2340d1d) == 1) {                        /* also log to the replay, encrypted */
        int len = 0x40;
        uint32_t h = *U(S, 0x2340d08);
        FUN_0046c0b0(S + 0x2340d29, 4, 1, h);
        FUN_0046c0b0(S + 0x234240d, 4, 1, h);
        FUN_0046c0b0(&len, 4, 1, h);
        FUN_0046c0b0(buf, 1, len, h);
    }
}
