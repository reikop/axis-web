/* Verify the complete homing-projectile lifecycle (axis_proj_update): it acquires
 * a target, homes onto it, and expires at its lifetime.
 *   cc axis_vecmath.c axis_proj.c axis_target.c axis_lifecycle_test.c -lm -o t && ./t
 */
#include <math.h>
#include <stdio.h>

typedef struct { float pos[3], vel[3]; int owner, target; float age, lifetime, speed, turnRate; } axis_proj_t;
int axis_proj_update(axis_proj_t *p, const float *actors, const int *active, int n, float maxRange, float dt);
float axis_vec_len(const float *v);

static int fails = 0;
static void ck(const char *w, int c) { if (!c) { printf("  FAIL %s\n", w); fails++; } }

int main(void) {
    const float dt = 1.0f/30.0f;

    /* one actor off to the side, within range (speed*lifetime); launched 90deg off */
    float actors[] = { 0, 800, 0 };
    int   active[] = { 1 };
    axis_proj_t p = { {0,0,0}, {120,0,0}, -1, -1, 0.0f, 10.0f, 120.0f, 3.0f /*rad/s*/ };

    int alive, reached = 0, ticks = 0;
    float startDist = axis_vec_len((float[]){actors[0]-p.pos[0], actors[1]-p.pos[1], actors[2]-p.pos[2]});
    while ((alive = axis_proj_update(&p, actors, active, 1, 0, dt))) {
        ticks++;
        float d[3] = { actors[0]-p.pos[0], actors[1]-p.pos[1], actors[2]-p.pos[2] };
        if (axis_vec_len(d) < p.speed*dt*1.5f) { reached = 1; break; }
        if (ticks > 100000) break;
    }
    ck("acquired the target", p.target == 0);
    ck("homed onto and reached the target", reached);
    ck("got closer than it started", axis_vec_len((float[]){actors[0]-p.pos[0], actors[1]-p.pos[1], actors[2]-p.pos[2]}) < startDist);
    ck("speed held during flight", fabsf(axis_vec_len(p.vel) - p.speed) < 1e-2f);

    /* lifetime expiry: no target, short life -> dies after ~lifetime/dt ticks */
    axis_proj_t q = { {0,0,0}, {100,0,0}, -1, -1, 0.0f, 1.0f, 100.0f, 0.0f };
    int qt = 0; while (axis_proj_update(&q, actors, (int[]){0}, 1, 0, dt)) { if (++qt > 100000) break; }
    ck("expires at its lifetime", qt >= 29 && qt <= 31);   /* 1.0s / (1/30) ≈ 30 */

    printf(fails ? "\nLIFECYCLE: %d FAILED\n" : "\nLIFECYCLE: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
