/* AXIS simulation — collision math + impact force.
 *
 * The projectile update (FUN_00479750) tests its swept path against the map with
 * FUN_00470bc0 (a 2932-byte triangle-soup intersection over the map collision
 * geometry) and, on impact, applies a knockback into the target actor's state:
 *     force = -direction * magnitude * 10   (SPEC §5, lines applying to 0x1ce1c04)
 *
 * The map-geometry traversal in FUN_00470bc0 needs the map collision structure
 * (a separate subsystem), but the primitive it is built on — segment vs triangle
 * — is standard and reconstructable + verifiable here. This provides that
 * primitive (Möller–Trumbore, clamped to the segment) and the impact force, as
 * clean original code with behavioural tests (axis_collide_test.c). The full
 * map-collision query layers these over the map's triangles once that structure
 * is reconstructed.
 */
#include <math.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

/* Segment p0->p1 vs triangle (a,b,c). On a hit within the segment, writes the
 * parametric distance t in [0,1] to *outT and the hit point to hit[3], returns 1;
 * else returns 0. (Möller–Trumbore ray/triangle, then clamp to the segment.) */
EXPORT
int axis_seg_tri(const float *p0, const float *p1, const float *a, const float *b,
                 const float *c, float *outT, float *hit) {
    float d[3]  = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
    float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
    float e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
    float pvec[3] = { d[1]*e2[2]-d[2]*e2[1], d[2]*e2[0]-d[0]*e2[2], d[0]*e2[1]-d[1]*e2[0] };
    float det = e1[0]*pvec[0] + e1[1]*pvec[1] + e1[2]*pvec[2];
    if (det > -1e-9f && det < 1e-9f) return 0;              /* parallel */
    float inv = 1.0f/det;
    float tvec[3] = { p0[0]-a[0], p0[1]-a[1], p0[2]-a[2] };
    float u = (tvec[0]*pvec[0]+tvec[1]*pvec[1]+tvec[2]*pvec[2]) * inv;
    if (u < 0.0f || u > 1.0f) return 0;
    float qvec[3] = { tvec[1]*e1[2]-tvec[2]*e1[1], tvec[2]*e1[0]-tvec[0]*e1[2], tvec[0]*e1[1]-tvec[1]*e1[0] };
    float v = (d[0]*qvec[0]+d[1]*qvec[1]+d[2]*qvec[2]) * inv;
    if (v < 0.0f || u + v > 1.0f) return 0;
    float t = (e2[0]*qvec[0]+e2[1]*qvec[1]+e2[2]*qvec[2]) * inv;
    if (t < 0.0f || t > 1.0f) return 0;                    /* outside the segment */
    *outT = t;
    hit[0] = p0[0] + d[0]*t; hit[1] = p0[1] + d[1]*t; hit[2] = p0[2] + d[2]*t;
    return 1;
}

/* Impact knockback: force imparted along `dir` (unit) scaled by `mag`, with the
 * engine's ×10 world scale and the sign the decompile uses (into -dir). */
EXPORT
void axis_impact_force(const float *dir, float mag, float *outForce) {
    float k = -mag * 10.0f;
    outForce[0] = dir[0]*k; outForce[1] = dir[1]*k; outForce[2] = dir[2]*k;
}
