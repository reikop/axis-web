/* Verify the homing-projectile steering (port/sim/axis_proj.c): a guided
 * projectile must turn toward its target, shrink the heading error each tick,
 * and converge onto it. No game ground-truth needed.
 *   cc axis_vecmath.c axis_proj.c axis_proj_test.c -lm -o t && ./t
 */
#include <math.h>
#include <stdio.h>

float axis_vec_len(const float *v);
void  axis_proj_steer(const float *vel, const float *pos, const float *target,
                      float maxTurn, float speed, float *outVel);
void  axis_proj_step(float *pos, float *vel, const float *target,
                     float maxTurn, float speed, float dt);

static int fails = 0;
static void ck(const char *w, int c) { if (!c) { printf("  FAIL %s\n", w); fails++; } }

static float headingErr(const float *vel, const float *pos, const float *target) {
    float to[3] = { target[0]-pos[0], target[1]-pos[1], target[2]-pos[2] };
    float tl = axis_vec_len(to), vl = axis_vec_len(vel);
    if (tl < 1e-6f || vl < 1e-6f) return 0.0f;
    float d = (vel[0]*to[0]+vel[1]*to[1]+vel[2]*to[2])/(tl*vl);
    if (d>1) d=1; else if (d<-1) d=-1;
    return acosf(d);
}

int main(void) {
    const float speed = 100.0f, maxTurn = 0.1f, dt = 1.0f/30.0f;

    /* 1) steering caps the turn at maxTurn and preserves speed */
    {
        float v[3]={speed,0,0}, p[3]={0,0,0}, tgt[3]={0,100,0}, nv[3];
        axis_proj_steer(v, p, tgt, maxTurn, speed, nv);
        float e0 = headingErr(v,p,tgt), e1 = headingErr(nv,p,tgt);
        ck("turn <= maxTurn (+eps)", (e0 - e1) <= maxTurn + 1e-4f);
        ck("speed preserved", fabsf(axis_vec_len(nv) - speed) < 1e-3f);
        ck("heading error decreased", e1 < e0);
    }

    /* 2) when already aligned, heading is unchanged */
    {
        float v[3]={speed,0,0}, p[3]={0,0,0}, tgt[3]={500,0,0}, nv[3];
        axis_proj_steer(v, p, tgt, maxTurn, speed, nv);
        ck("aligned -> straight", fabsf(nv[0]-speed)<1e-2f && fabsf(nv[1])<1e-2f && fabsf(nv[2])<1e-2f);
    }

    /* 3) full homing run converges: a projectile launched 90deg off a fixed
     *    target reduces its heading error monotonically to ~0 and reaches it */
    {
        float p[3]={0,0,0}, v[3]={speed,0,0};
        const float tgt[3]={0,3000,0};
        float prevErr = headingErr(v,p,tgt); int mono=1, hit=0;
        for (int i=0;i<2000;i++) {
            axis_proj_step(p, v, tgt, maxTurn, speed, dt);
            float e = headingErr(v,p,tgt);
            float d[3]={tgt[0]-p[0],tgt[1]-p[1],tgt[2]-p[2]};
            if (axis_vec_len(d) < speed*dt*1.5f) { hit=1; break; }
            if (e > prevErr + 1e-3f) mono=0;      /* allow tiny overshoot past target only */
            prevErr = e;
        }
        ck("homing heading error monotonically shrinks", mono);
        ck("homing reaches the target", hit);
    }

    printf(fails ? "\nPROJ: %d FAILED\n" : "\nPROJ: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
