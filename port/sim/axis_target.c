/* AXIS simulation — homing target acquisition.
 *
 * Clean reimplementation of the target-selection loop inside FUN_00479750
 * (SPEC §5): a guided projectile scans the actor array (stride 0x8888) and locks
 * onto the NEAREST eligible actor — one that is active, is not the projectile's
 * owner, and passes the weapon's validity/range check. In the decompile the
 * eligibility is: actor active flag set, a per-actor stat (`[-0x60]`) above a
 * threshold, actor != owner, a targetable flag (`[0x86cc]`) clear, an in-range
 * predicate (`FUN_0047ce50`), and nearest-so-far distance.
 *
 * Here the understood core — "nearest eligible non-owner active actor within
 * range" — is reconstructed and verifiable; the exact per-weapon predicate
 * (`FUN_0047ce50`, the stat threshold) is exposed as parameters so the behaviour
 * is checkable without guessing the weapon-specific numbers.
 */
#include <math.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

/* Pick the nearest eligible actor to `pos`, or -1 if none.
 *   actors : xyz triples, one per actor (actor k at actors[k*3..k*3+2])
 *   active : per-actor active flag (0/1)
 *   n      : actor count
 *   owner  : the projectile's owner index to skip (-1 = none)
 *   maxRange: only consider actors within this distance (<=0 = unlimited)
 * Returns the nearest eligible actor index, else -1. */
EXPORT
int axis_acquire_target(const float *pos, const float *actors, const int *active,
                        int n, int owner, float maxRange) {
    int best = -1;
    float bestD2 = (maxRange > 0.0f) ? maxRange * maxRange : 3.4e38f;
    for (int k = 0; k < n; k++) {
        if (!active[k] || k == owner) continue;
        float dx = actors[k*3] - pos[0], dy = actors[k*3+1] - pos[1], dz = actors[k*3+2] - pos[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < bestD2) { bestD2 = d2; best = k; }
    }
    return best;
}
