/* AXIS animated-prop / interactive-scenery update — reconstructed from the
 * type-C per-entity body (FUN_0048f8d0, dispatched by FUN_004901d0; record
 * stride 0x2cc, array 0x131ae48, count 0x13c9b24). Type C drives animated and
 * interactive set pieces: doors, lifts/platforms, proximity triggers and
 * screen-space glow sprites (lens flares). The whole body also rebuilds the
 * prop's local->world matrices and draws it, which is engine/render work; the
 * pieces reconstructed here are the ones that are pure logic / kinematics and
 * therefore verifiable offline with no live-match ground truth:
 *
 *   (1) Animation phase advance with loop modes. A prop carries a phase in
 *       [0,1] and a rate; each tick phase += dt*rate. Mode 1 (CLAMP) holds at
 *       the top and stops+notifies at the bottom; mode 3 (ONESHOT) resets to 0
 *       and stops+notifies at either end. (Mode 2 re-rolls the phase through an
 *       x87 float10 expression that cannot be reproduced bit-exactly in WASM,
 *       so it is left to the caller and not reconstructed here.)
 *
 *   (2) Proximity activation. The radius is range*100 (range = record +0x10);
 *       the prop opens (plays forward) when the nearest active actor is inside
 *       that radius and closes (plays reverse) when the nearest leaves. The
 *       comparison is on squared distance, matching the binary's actor scan.
 *
 *   (3) Glow visibility fade. A glow's alpha fades toward 1 at 4/sec while it is
 *       on-screen and unoccluded, toward 0 otherwise, clamped to [0,1].
 *
 *   (4) Clip-space frustum visibility — the standard -w<=x,y,z<=w test on a
 *       projected homogeneous point (the binary's cull before the fade).
 *
 * Clean original code, not a transcription of decompiler output. Verified by
 * axis_prop_test.c against these identities. X-Z ground / Y up (SPEC §4).
 */
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

#define AXIS_GLOW_FADE_RATE 4.0f    /* glow alpha change per second (FUN_0048f8d0) */
#define AXIS_PROP_RANGE_UNIT 100.0f /* record +0x10 range is in 100-unit steps     */

enum { AXIS_ANIM_CLAMP = 1, AXIS_ANIM_RANDOM = 2, AXIS_ANIM_ONESHOT = 3 };

/* Advance an animation phase one tick.
 *   phase,*rate : current phase in [0,1] and its signed rate (per second)
 *   mode        : AXIS_ANIM_CLAMP (1) or AXIS_ANIM_ONESHOT (3); other modes
 *                 (incl. 2) advance without end handling here
 *   armed       : whether an end-of-animation callback is armed (record +0x28c)
 *   *callback   : set to 1 when the end callback should fire this tick
 * On reaching an end the rate is zeroed (the prop stops). Returns the new phase. */
EXPORT
float axis_anim_phase_step(float phase, float *rate, int mode, float dt, int armed, int *callback) {
    float p = phase + dt * (*rate);
    if (callback) *callback = 0;
    if (mode == AXIS_ANIM_CLAMP) {
        if (p >= 0.0f) {
            if (p > 1.0f) { p = 1.0f; *rate = 0.0f; }   /* hold fully open */
        } else {
            p = 0.0f; *rate = 0.0f;                      /* closed: stop + notify */
            if (armed && callback) *callback = 1;
        }
    } else if (mode == AXIS_ANIM_ONESHOT) {
        if (p < 0.0f || p > 1.0f) {                      /* reset at either end */
            p = 0.0f; *rate = 0.0f;
            if (armed && callback) *callback = 1;
        }
    }
    return p;
}

/* Squared activation radius for a prop whose range field (record +0x10) is the
 * given number of 100-unit steps. */
EXPORT
float axis_prop_radius2(int rangeSteps) {
    float r = (float)rangeSteps * AXIS_PROP_RANGE_UNIT;
    return r * r;
}

/* Whether the nearest actor (its squared distance to the prop) is inside the
 * prop's activation radius. The binary stays idle while radius^2 <= dist^2, so
 * "inside" is strictly dist^2 < radius^2. */
EXPORT
int axis_prop_inside(float dist2, int rangeSteps) {
    return dist2 < axis_prop_radius2(rangeSteps);
}

/* Nearest squared distance from a prop position to any active actor.
 *   actors : xyz triples, one per actor
 *   active : per-actor active flag (0 = skip)
 * Returns a huge value when no actor is active. Mirrors the record's actor
 * scan (active actors only, squared distance, minimum). */
EXPORT
float axis_prop_nearest_dist2(const float *propPos, const float *actors, const int *active, int n) {
    float best = 3.4028235e+38f;
    for (int i = 0; i < n; i++) {
        if (!active[i]) continue;
        float dx = actors[i*3+0] - propPos[0];
        float dy = actors[i*3+1] - propPos[1];
        float dz = actors[i*3+2] - propPos[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < best) best = d2;
    }
    return best;
}

/* One glow-alpha fade tick: ramp toward 1 while visible+unoccluded, toward 0
 * otherwise, at 4/sec, clamped to [0,1]. */
EXPORT
float axis_glow_fade(float alpha, float dt, int visibleUnoccluded) {
    if (visibleUnoccluded) {
        alpha += dt * AXIS_GLOW_FADE_RATE;
        if (alpha > 1.0f) alpha = 1.0f;
    } else {
        alpha -= dt * AXIS_GLOW_FADE_RATE;
        if (alpha < 0.0f) alpha = 0.0f;
    }
    return alpha;
}

/* Standard clip-space visibility: a projected homogeneous point (x,y,z,w) is
 * on-screen when -w <= x,y,z <= w. The binary culls when any bound is crossed
 * (it tests with <=, so points exactly on a far bound are treated as culled). */
EXPORT
int axis_clip_visible(float x, float y, float z, float w) {
    float negw = -w;
    if (x <= negw || y <= negw || z <= negw) return 0;
    if (w <= x || w <= y || w <= z) return 0;
    return 1;
}
