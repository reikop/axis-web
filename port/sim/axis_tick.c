/* AXIS simulation — runnable per-tick loop (projectile subsystem).
 *
 * The decompiled per-tick dispatchers (FUN_0047c480 etc., reconstructed in
 * axis_sim.c) walk an entity array each frame and run the per-entity update. This
 * is the integrated, runnable form for the reconstructed+verified projectile
 * subsystem: advance every live projectile one combat tick (home, swept hit,
 * knockback, consume) against the actor array, accumulating per-actor impact
 * forces and recording hits. It ties the verified layers (axis_vecmath, axis_proj,
 * axis_target, axis_collide) into one `sim_tick` the engine can call per frame.
 *
 * Clean original code; verified by axis_tick_test.c driving a multi-projectile,
 * multi-actor scene (no game ground-truth needed — behavioural checks only).
 */
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

typedef struct {
    float pos[3], vel[3];
    int   owner, target;
    float age, lifetime, speed, turnRate;
} axis_proj_t;

int axis_proj_combat(axis_proj_t *p, const float *actors, float *forces, const int *active,
                     int n, float radius, float mag, float maxRange, float dt);

/* Run one simulation tick over the projectile subsystem.
 *   projs[nProj] : the projectile entities (alive flagged by palive[])
 *   palive[]     : per-projectile alive flag (set 0 when a projectile is spent)
 *   actors/aalive/nActors : the actor array (xyz triples) + active flags
 *   forces[nActors*3]     : per-actor impact-force accumulator (added to)
 *   radius,mag,maxRange,dt: hit radius, knockback magnitude, lock range, timestep
 * Returns the number of actor hits this tick. */
EXPORT
int axis_sim_tick(axis_proj_t *projs, int *palive, int nProj,
                  const float *actors, const int *aalive, int nActors,
                  float *forces, float radius, float mag, float maxRange, float dt) {
    int hits = 0;
    for (int i = 0; i < nProj; i++) {
        if (!palive[i]) continue;
        int r = axis_proj_combat(&projs[i], actors, forces, aalive, nActors, radius, mag, maxRange, dt);
        if (r == -2) { palive[i] = 0; }          /* expired, no hit */
        else if (r >= 0) { palive[i] = 0; hits++; }   /* hit an actor and was consumed */
    }
    return hits;
}
