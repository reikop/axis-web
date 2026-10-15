/* Verify the integrated simulation tick (axis_sim_tick): several homing
 * projectiles drive to their targets over many ticks; each eventually hits an
 * actor, the actor accumulates knockback, and the projectile is retired.
 *   cc axis_vecmath.c axis_proj.c axis_target.c axis_collide.c axis_tick.c axis_tick_test.c -lm -o t && ./t
 */
#include <math.h>
#include <stdio.h>

typedef struct { float pos[3], vel[3]; int owner, target; float age, lifetime, speed, turnRate; } axis_proj_t;
int axis_sim_tick(axis_proj_t *projs, int *palive, int nProj,
                  const float *actors, const int *aalive, int nActors,
                  float *forces, float radius, float mag, float maxRange, float dt);
float axis_vec_len(const float *v);

static int fails=0; static void ck(const char*w,int c){if(!c){printf("  FAIL %s\n",w);fails++;}}

int main(void){
    const float dt=1.0f/30.0f;
    /* three actors spread around; three projectiles fired from the origin in
       different directions, each should home onto and hit an actor */
    float actors[] = { 600,0,0,   0,600,0,   0,0,600 };
    int   aalive[] = { 1, 1, 1 };
    float forces[9] = {0};

    axis_proj_t projs[3] = {
        { {0,0,0}, {0,150,0},  -1, 1, 0, 12, 150, 4 },   /* -> actor1 (0,600,0), fired +y */
        { {0,0,0}, {150,0,0},  -1, 0, 0, 12, 150, 4 },   /* -> actor0 (600,0,0), fired +x */
        { {0,0,0}, {0,0,150},  -1, 2, 0, 12, 150, 4 },   /* -> actor2 (0,0,600), fired +z */
    };
    int palive[3] = {1,1,1};

    int totalHits = 0, ticks = 0, anyLive = 1;
    while (anyLive && ticks < 100000) {
        totalHits += axis_sim_tick(projs, palive, 3, actors, aalive, 3, forces, 30.0f, 5.0f, 0.0f, dt);
        ticks++;
        anyLive = palive[0] || palive[1] || palive[2];
    }
    ck("all three projectiles resolved (none stuck flying)", !anyLive);
    ck("three actor hits registered", totalHits == 3);
    /* every actor took some knockback */
    int actorsHit = 0;
    for (int k = 0; k < 3; k++) if (axis_vec_len(forces + k*3) > 0.0f) actorsHit++;
    ck("all three actors received knockback", actorsHit == 3);

    printf(fails?"\nTICK: %d FAILED\n":"\nTICK: ALL PASS\n", fails);
    return fails?1:0;
}
