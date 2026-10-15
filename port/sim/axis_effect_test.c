/* Verify the effect/debris layer (axis_effect.c) against the identities the
 * reconstruction must satisfy — no game ground-truth required:
 *   - intensity curve: K at t=0, 0 at t=1, monot- non-increasing, = (1-t)^2*K,
 *     and 0 for types outside the table (incl. type 9);
 *   - gravity debris free flight: horizontal advance = N*dt*vel, vertical
 *     displacement = N*dt*(vel.y - G); expiry past lifetime;
 *   - trail cadence ~ every 0.1s; trail-kind split ~1/10 kind-2 over the
 *     verified MSVC determinism pool, and deterministic per value.
 *   cc axis_effect.c axis_effect_test.c -lm -o t && ./t
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>

#define GRAV 2000.0f

typedef struct { float pos[3], vel[3], age, lifetime, trailTimer; } axis_debris_t;
enum { FLYING = 0, HIT = 1, EXPIRED = 2 };

float axis_effect_intensity(float age, float lifetime, int type);
int   axis_debris_trail_kind(uint32_t poolval);
int   axis_debris_step(axis_debris_t *d, float dt,
                       int (*collide)(const float*, float*, float*, void*), void*,
                       uint32_t (*draw_pool)(void*), void*,
                       void (*trail)(int, uint32_t, void*), void*);

static int fails = 0;
static void ck(const char *w, int c) { if (!c) { printf("  FAIL %s\n", w); fails++; } }
static int approx(float a, float b, float eps) { float d = a - b; if (d < 0) d = -b + a; return (d < 0 ? -d : d) < eps; }

/* the verified shared pool = MSVC rand() after srand(1) (SPEC §5) */
static uint32_t g_seed = 1;
static uint32_t msvc_rand(void) { g_seed = g_seed * 214013u + 2531011u; return (g_seed >> 16) & 0x7fff; }
static uint32_t draw_pool_cb(void *ctx) { (void)ctx; return msvc_rand(); }

/* a fixed pool that never selects kind 2 (keeps debris alive for the cadence test) */
static uint32_t draw_pool_k0(void *ctx) { (void)ctx; return 7; /* 7%10 != 0 */ }

static int trail_count = 0, trail_kind2 = 0;
static void trail_cb(int kind, uint32_t v, void *ctx) { (void)v; (void)ctx; trail_count++; if (kind == 2) trail_kind2++; }

/* a collider that reports a hit crossing the ground plane y = 0 */
static int collide_ground(const float *from, float *to, float *hit, void *ctx) {
    (void)ctx;
    if (from[1] >= 0.0f && to[1] < 0.0f) {
        float f = from[1] / (from[1] - to[1]);   /* fraction to y=0 */
        hit[0] = from[0] + f * (to[0] - from[0]);
        hit[1] = 0.0f;
        hit[2] = from[2] + f * (to[2] - from[2]);
        return 1;
    }
    return 0;
}

int main(void) {
    /* --- (1) intensity curve --- */
    ck("type 2 K at t=0 == 3000", approx(axis_effect_intensity(0, 10, 2), 3000.0f, 1e-2f));
    ck("type 3 K at t=0 == 5000", approx(axis_effect_intensity(0, 10, 3), 5000.0f, 1e-2f));
    ck("type 0x20 K at t=0 == 1500", approx(axis_effect_intensity(0, 10, 0x20), 1500.0f, 1e-2f));
    ck("intensity 0 at t=1", approx(axis_effect_intensity(10, 10, 2), 0.0f, 1e-3f));
    ck("type outside table -> 0", axis_effect_intensity(0, 10, 99) == 0.0f);
    ck("type 9 (debris) has no curve -> 0", axis_effect_intensity(0, 10, 9) == 0.0f);
    /* exact quadratic shape + monotone non-increasing */
    {
        int mono = 1; float prev = 1e30f;
        for (int i = 0; i <= 20; i++) {
            float t = i / 20.0f, age = t * 10.0f;
            float got = axis_effect_intensity(age, 10, 3);     /* K=5000 */
            float want = (1.0f - t) * (1.0f - t) * 5000.0f;
            if (!approx(got, want, 1e-2f)) { mono = 0; }
            if (got > prev + 1e-3f) mono = 0;
            prev = got;
        }
        ck("intensity matches (1-t)^2*K and is non-increasing", mono);
    }

    /* --- (2) gravity debris free flight --- */
    {
        axis_debris_t d = { {0,1000,0}, {30,10,-20}, 0, 1e9f, 1.0f };
        float dt = 1.0f / 30.0f; int N = 60, st = FLYING;
        for (int i = 0; i < N; i++) st = axis_debris_step(&d, dt, NULL, NULL, NULL, NULL, NULL, NULL);
        ck("free flight stays flying", st == FLYING);
        ck("x advanced by N*dt*vx", approx(d.pos[0], N * dt * 30.0f, 1.0f));
        ck("z advanced by N*dt*vz", approx(d.pos[2], N * dt * -20.0f, 1.0f));
        /* vertical: each step adds dt*vy then sinks dt*G -> dt*(vy-G) */
        ck("y displaced by N*dt*(vy-G)", approx(d.pos[1] - 1000.0f, N * dt * (10.0f - GRAV), 2.0f));
    }

    /* --- expiry past lifetime --- */
    {
        axis_debris_t d = { {0,0,0}, {0,0,0}, 2.0f, 1.0f, 1.0f };   /* age 2 > lifetime 1 */
        int st = axis_debris_step(&d, 0.033f, NULL, NULL, NULL, NULL, NULL, NULL);
        ck("age past lifetime -> EXPIRED", st == EXPIRED);
    }

    /* --- ground collision sticks the debris --- */
    {
        axis_debris_t d = { {0,5,0}, {0,-1000,0}, 0, 1e9f, 1.0f };  /* falling fast */
        int st = FLYING;
        for (int i = 0; i < 10 && st == FLYING; i++) st = axis_debris_step(&d, 1.0f/30.0f, collide_ground, NULL, NULL, NULL, NULL, NULL);
        ck("debris hits the ground plane", st == HIT);
        ck("stuck at y=0", approx(d.pos[1], 0.0f, 1e-3f));
    }

    /* --- (3) trail cadence ~ every 0.1s --- */
    {
        trail_count = 0; trail_kind2 = 0;
        axis_debris_t d = { {0,1e6f,0}, {0,0,0}, 0, 1e9f, 0.1f };   /* trailTimer starts at the 0.1s period */
        float dt = 1.0f / 30.0f;
        for (int i = 0; i < 30; i++)   /* 1.0 s */
            axis_debris_step(&d, dt, NULL, NULL, draw_pool_k0, NULL, trail_cb, NULL);
        ck("~10 trail emissions over 1s (every 0.1s)", trail_count >= 9 && trail_count <= 11);
    }

    /* --- trail kind: deterministic + ~1/10 kind-2 over the verified pool --- */
    {
        ck("trail kind 2 when pool%10==0", axis_debris_trail_kind(100) == 2);
        ck("trail kind 0 otherwise", axis_debris_trail_kind(101) == 0);
        g_seed = 1; int k2 = 0, Nn = 100000;
        for (int i = 0; i < Nn; i++) if (axis_debris_trail_kind(msvc_rand()) == 2) k2++;
        float frac = (float)k2 / Nn;
        ck("~1/10 of pool draws pick kind 2", frac > 0.08f && frac < 0.12f);
        (void)draw_pool_cb;
    }

    printf(fails ? "\nEFFECT: %d FAILED\n" : "\nEFFECT: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
