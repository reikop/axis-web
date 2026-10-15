/* Verify the complete projectile combat loop (axis_proj_combat): a homing
 * projectile acquires a target, flies to it, registers a swept hit on the actor,
 * applies forward knockback into that actor, and is consumed.
 *   cc axis_vecmath.c axis_proj.c axis_target.c axis_collide.c axis_combat_test.c -lm -o t && ./t
 */
#include <math.h>
#include <stdio.h>

typedef struct { float pos[3], vel[3]; int owner, target; float age, lifetime, speed, turnRate; } axis_proj_t;
int   axis_proj_combat(axis_proj_t *p, const float *actors, float *forces, const int *active,
                       int n, float radius, float mag, float maxRange, float dt);
float axis_vec_len(const float *v);

static int fails=0; static void ck(const char*w,int c){if(!c){printf("  FAIL %s\n",w);fails++;}}

int main(void){
    const float dt=1.0f/30.0f;
    float actors[] = { 0, 600, 0 };          /* one target, within range */
    int   active[] = { 1 };
    float forces[] = { 0,0,0 };
    axis_proj_t p = { {0,0,0}, {150,0,0}, -1, -1, 0.0f, 10.0f, 150.0f, 4.0f };

    int hitActor=-99, r, ticks=0;
    while ((r = axis_proj_combat(&p, actors, forces, active, 1, 30.0f, 5.0f, 0.0f, dt)) == -1) {
        if (++ticks > 100000) break;
    }
    ck("projectile hit the target actor", r == 0);
    hitActor = r;
    ck("knockback applied to the hit actor", axis_vec_len(forces) > 0.0f);
    /* the knockback should point roughly toward the target from the launch side
       (the projectile arrives travelling mostly +y), so force y-component > 0 */
    ck("knockback is forward (+y) along travel", forces[1] > 0.0f);
    ck("projectile consumed on hit (age>=lifetime)", p.age >= p.lifetime);
    (void)hitActor;

    /* a projectile with no reachable target expires with no hit (-2) */
    axis_proj_t q = { {0,0,0}, {150,0,0}, -1, -1, 0.0f, 0.5f, 150.0f, 0.0f };
    int qr=-1, qt=0; float qf[3]={0,0,0};
    int none[]={0};
    while ((qr = axis_proj_combat(&q, actors, qf, none, 1, 30.0f, 5.0f, 0.0f, dt)) == -1) { if(++qt>100000) break; }
    ck("no-target projectile expires without hit", qr == -2);

    printf(fails?"\nCOMBAT: %d FAILED\n":"\nCOMBAT: ALL PASS\n", fails);
    return fails?1:0;
}
