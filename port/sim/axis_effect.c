/* AXIS effect / debris entity update — reconstructed from the type-A per-entity
 * body (FUN_00475890, dispatched by FUN_00478f10). Unlike the projectile sim
 * (axis_proj/axis_tick, from FUN_00479750) this subsystem drives *timed visual
 * entities*: muzzle flashes, explosions, sparks and gravity-thrown debris. Two
 * behaviours can be reconstructed cleanly and verified offline (no live-match
 * ground truth needed), because they are pure math / pure kinematics:
 *
 *   (1) Timed-effect intensity. Every effect carries an age and a lifetime; its
 *       drawn intensity (radius / brightness, the value written to the record's
 *       +0x7c slot) follows a quadratic ease-out
 *            intensity = (1 - t)^2 * K(type),   t = clamp(age / lifetime, 0..1)
 *       where K(type) is a fixed per-effect-type magnitude table read straight
 *       out of the binary's switch(type/5) ladder.
 *
 *   (2) Type-9 gravity debris. A ballistic step: the position advances by
 *       dt*velocity and additionally sinks dt*G on the vertical (Y) axis with
 *       G = 2000 (the literal from the decompile); the velocity itself is NOT
 *       re-integrated, so vertical motion is the constant rate (vel.y - G) — a
 *       straight fall, reproduced faithfully rather than "corrected" to a
 *       parabola. A pluggable collision test can stop/stick the debris at an
 *       impact point. A trail timer fires every 0.1s (0x3dcccccd); each firing
 *       draws one value from the shared determinism pool and emits trail kind 2
 *       when (value % 10 == 0), else kind 0.
 *
 * Clean original code, not a transcription of decompiler output. Verified by
 * axis_effect_test.c against these identities; the X–Z-ground, Y-up convention
 * matches SPEC §4.
 */
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

#define AXIS_EFFECT_GRAVITY   2000.0f   /* vertical sink rate (FUN_00475890 literal) */
#define AXIS_EFFECT_TRAIL_DT  0.1f      /* trail-emission period, 0x3dcccccd         */

/* Per-effect-type intensity magnitude K. Mirrors the switch(type/5) ladder:
 * every type not listed has no intensity curve (the binary leaves +0x7c
 * untouched), so we report 0 for those (including the type-9 debris, whose
 * draw is handled by the debris path, not this curve). */
static float effect_magnitude(int type) {
    switch (type) {
    case 0x02:                      return 3000.0f;
    case 0x03:                      return 5000.0f;
    case 0x19: case 0x1a:           return 2000.0f;
    case 0x1e: case 0x1f:           return 1000.0f;
    case 0x20: case 0x21: case 0x22:return 1500.0f;
    case 0x23:                      return 5000.0f;
    case 0x24:                      return 3000.0f;
    case 0x25: case 0x26:           return 1500.0f;
    default:                        return 0.0f;
    }
}

/* Drawn intensity of a timed effect at the given age. t is clamped to [0,1]
 * (the caller despawns once age exceeds lifetime, so t never exceeds 1 in
 * normal play; we clamp anyway for safety). Returns 0 for types with no curve. */
EXPORT
float axis_effect_intensity(float age, float lifetime, int type) {
    float t, u, k = effect_magnitude(type);
    if (k == 0.0f) return 0.0f;
    if (lifetime <= 0.0f) t = 0.0f;
    else {
        t = age / lifetime;
        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;
    }
    u = 1.0f - t;
    return u * u * k;
}

/* A gravity-debris entity (type-9). Field layout mirrors the record slots the
 * binary touches, but as a plain struct the web engine can own directly. */
typedef struct {
    float pos[3];       /* +0x10 world position (X, Y up, Z)       */
    float vel[3];       /* +0x1c constant velocity (not re-integrated) */
    float age;          /* +0x74 elapsed time                      */
    float lifetime;     /* +0x70 total lifetime                    */
    float trailTimer;   /* +0x84 countdown to next trail emission  */
} axis_debris_t;

enum { AXIS_DEBRIS_FLYING = 0, AXIS_DEBRIS_HIT = 1, AXIS_DEBRIS_EXPIRED = 2 };

/* Trail kind chosen for one emission from a shared-pool value: kind 2 (a bright
 * impact-style spark, with lifetime reset) when value%10==0, else kind 0. */
EXPORT
int axis_debris_trail_kind(uint32_t poolval) {
    return (poolval % 10u == 0u) ? 2 : 0;
}

/* Advance one type-9 debris entity by dt.
 *   collide : optional geometry test; returns non-zero and writes the impact
 *             point into hit[3] when the segment from->to crosses geometry.
 *             Pass NULL for free flight. ctx is forwarded to it.
 *   trail   : optional sink called once per trail emission with the kind
 *             (0 or 2) and the drawn pool value; pass NULL to ignore.
 *   draw_pool : returns the next shared-pool value (pool[idx++ & 0xfff]); pass
 *             NULL to disable trail emission entirely.
 * Returns AXIS_DEBRIS_EXPIRED (lifetime elapsed -> despawn), AXIS_DEBRIS_HIT
 * (bounced/stuck at an impact point this step) or AXIS_DEBRIS_FLYING. */
EXPORT
int axis_debris_step(axis_debris_t *d, float dt,
                     int (*collide)(const float *from, float *to, float *hit, void *ctx),
                     void *ctx,
                     uint32_t (*draw_pool)(void *ctx2), void *ctx2,
                     void (*trail)(int kind, uint32_t poolval, void *ctx3), void *ctx3) {
    int status = AXIS_DEBRIS_FLYING;

    /* despawn once the entity has outlived its lifetime (checked on the age
     * carried into this tick, before it is advanced) */
    if (d->lifetime < d->age) return AXIS_DEBRIS_EXPIRED;

    /* candidate position: straight advance by dt*vel, with the vertical axis
     * additionally sinking dt*G (gravity applied to position, velocity held) */
    float from[3] = { d->pos[0], d->pos[1], d->pos[2] };
    float to[3];
    to[0] = d->pos[0] + dt * d->vel[0];
    to[1] = d->pos[1] + dt * d->vel[1];
    to[2] = d->pos[2] + dt * d->vel[2];
    float sunkY = to[1] - dt * AXIS_EFFECT_GRAVITY;

    float hit[3];
    if (collide && collide(from, to, hit, ctx)) {
        /* struck geometry: stick at the impact point and fully fade (lifetime
         * collapses to the current age so the effect reads t=1 next frame) */
        d->pos[0] = hit[0]; d->pos[1] = hit[1]; d->pos[2] = hit[2];
        d->lifetime = d->age;
        status = AXIS_DEBRIS_HIT;
    } else {
        d->pos[0] = to[0];
        d->pos[1] = sunkY;      /* gravity-sunk vertical */
        d->pos[2] = to[2];
    }

    /* trail emission on a fixed 0.1s cadence */
    d->trailTimer -= dt;
    if (d->trailTimer <= 0.0f) {
        if (draw_pool) {
            uint32_t v = draw_pool(ctx2);
            int kind = axis_debris_trail_kind(v);
            if (trail) trail(kind, v, ctx3);
            if (kind == 2) d->lifetime = d->age;   /* kind-2 trail resets fade */
        }
        d->trailTimer = AXIS_EFFECT_TRAIL_DT;
    }

    d->age += dt;
    return status;
}
