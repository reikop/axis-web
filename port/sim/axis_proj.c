/* AXIS simulation — homing-projectile steering + integration.
 *
 * This is a CLEAN reimplementation (original code, built on the verified
 * primitives in axis_vecmath.c) of the core flight behaviour inside the
 * projectile/weapon-entity update FUN_00479750 (SPEC §5): a guided projectile
 * steers its velocity toward a target by a capped turn angle each tick, then
 * advances. In the decompile the turn is done by building the perpendicular axis
 * from (direction-to-target × velocity), rotating the velocity direction toward
 * the target by `turn` (a per-weapon turn rate, radians), and rescaling to the
 * projectile's speed; then the position integrates by the frame velocity.
 *
 * It is the behaviour-level reconstruction of the homing core — the part that can
 * be understood cleanly and *checked* (a homing projectile must reduce the angle
 * to its target and converge; straight flight when already aligned or turn=0).
 * The surrounding concerns in FUN_00479750 that aren't physics (trail-billboard
 * GL rendering, effect spawning, target-list scan, the collision callee) stay out
 * of this module. No game ground-truth is needed to verify the steering law
 * itself; see axis_proj_test.c. x87-precision caveat per SPEC §5.
 */
#include <math.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

float axis_vec_len(const float *v);
float axis_vec_normalize(const float *v, float *out);
void  axis_rotate_axis_angle(const float *v, const float *axis, float angle, float *out);

/* Steer `vel` (current velocity) toward `target` from `pos`, turning at most
 * `maxTurn` radians this step, and rescale to `speed`; result in `outVel`. */
EXPORT
void axis_proj_steer(const float *vel, const float *pos, const float *target,
                     float maxTurn, float speed, float *outVel) {
    float to[3] = { target[0] - pos[0], target[1] - pos[1], target[2] - pos[2] };
    float td[3], vd[3];
    if (axis_vec_normalize(to, td) < 1e-6f || axis_vec_len(vel) < 1e-6f) {
        /* no target offset or no current heading: keep heading, just set speed */
        axis_vec_normalize(vel, vd);
        outVel[0] = vd[0] * speed; outVel[1] = vd[1] * speed; outVel[2] = vd[2] * speed;
        return;
    }
    axis_vec_normalize(vel, vd);
    float dot = vd[0]*td[0] + vd[1]*td[1] + vd[2]*td[2];
    if (dot > 1.0f) dot = 1.0f; else if (dot < -1.0f) dot = -1.0f;
    float ang = acosf(dot);                         /* angle between heading and target dir */
    float turn = ang < maxTurn ? ang : maxTurn;     /* cap the turn */
    /* rotation axis = heading × target-dir; rotating vd about it by +turn moves
     * vd toward td. (If exactly anti-parallel, pick any perpendicular axis.) */
    float axis[3] = { vd[1]*td[2] - vd[2]*td[1], vd[2]*td[0] - vd[0]*td[2], vd[0]*td[1] - vd[1]*td[0] };
    float k[3]; float al = axis_vec_normalize(axis, k);
    float nd[3];
    if (al < 1e-6f) {                               /* parallel (turn 0) or anti-parallel */
        if (dot > 0.0f) { nd[0]=vd[0]; nd[1]=vd[1]; nd[2]=vd[2]; }
        else { float up[3]={0,1,0}; float a2[3]; axis_vec_normalize((float[]){vd[1]*up[2]-vd[2]*up[1], vd[2]*up[0]-vd[0]*up[2], vd[0]*up[1]-vd[1]*up[0]}, a2);
               axis_rotate_axis_angle(vd, a2, turn, nd); }
    } else {
        axis_rotate_axis_angle(vd, k, turn, nd);
    }
    outVel[0] = nd[0]*speed; outVel[1] = nd[1]*speed; outVel[2] = nd[2]*speed;
}

/* Advance a projectile one tick: steer toward target then integrate position by
 * vel*dt. Writes new pos+vel in place. Returns remaining life (age vs lifetime
 * handled by the caller; this is the motion step). */
EXPORT
void axis_proj_step(float *pos, float *vel, const float *target,
                    float maxTurn, float speed, float dt) {
    float nv[3]; axis_proj_steer(vel, pos, target, maxTurn, speed, nv);
    vel[0]=nv[0]; vel[1]=nv[1]; vel[2]=nv[2];
    pos[0]+=vel[0]*dt; pos[1]+=vel[1]*dt; pos[2]+=vel[2]*dt;
}
