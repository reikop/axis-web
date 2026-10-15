/* AXIS collision broad phase — reconstructed from FUN_00470bc0, the first half
 * of the collision query that the projectile sim (FUN_00479750) and the debris /
 * prop updaters (axis_effect / axis_prop) all call. Given a motion segment
 * (from -> to) in world space, FUN_00470bc0 walks every collidable prop (the
 * type-C array, count 0x13c9b24, stride 0xb3 floats = 0x2cc bytes), transforms
 * the two segment endpoints into that prop's local space by its inverse matrix,
 * and runs a 3-D Cohen-Sutherland trivial-reject + clip of the segment against
 * the prop's local axis-aligned bounding box (min at record +0x34/0x38/0x3c,
 * max at +0x40/0x44/0x48). Props whose box the segment can still reach are
 * collected as candidates and handed to the narrow phase FUN_004824e0 (the
 * actual triangle test; our axis_collide.c axis_seg_tri is that layer). The
 * broad-phase math is the standard, fully offline-verifiable part, reconstructed
 * here as clean original C:
 *
 *   - affine point transform (world -> prop local): p' = M*p + T;
 *   - the 6-bit AABB outcode with the binary's exact bit layout and its
 *     closed-exterior boundaries (a point on a face counts as outside);
 *   - Cohen-Sutherland trivial reject: a segment misses the box when its two
 *     endpoints share an outside half-space (outcode(a) & outcode(b) != 0);
 *   - a definitive slab-method segment/AABB intersection (entry parameter in
 *     [0,1]) for the accept side.
 *
 * Clean original code, not a transcription of decompiler output. Verified by
 * axis_broadphase_test.c against geometric identities.
 */
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

/* outcode bits (match FUN_00470bc0): low bit = at/below min, high bit = at/above max */
#define AXIS_OC_XMIN 0x000001
#define AXIS_OC_XMAX 0x000010
#define AXIS_OC_YMIN 0x000100
#define AXIS_OC_YMAX 0x001000
#define AXIS_OC_ZMIN 0x010000
#define AXIS_OC_ZMAX 0x100000

/* Transform a world point into a prop's local space: out = M*p + T, with M a
 * row-major 3x3 and T a translation. (In the binary the basis columns are
 * interleaved in the prop record at a 0xb3-float stride; the operation is a
 * plain affine transform, which is what matters and what we verify.) */
EXPORT
void axis_xform_affine(float *out, const float *M, const float *T, const float *p) {
    float x = p[0], y = p[1], z = p[2];
    out[0] = M[0]*x + M[1]*y + M[2]*z + T[0];
    out[1] = M[3]*x + M[4]*y + M[5]*z + T[1];
    out[2] = M[6]*x + M[7]*y + M[8]*z + T[2];
}

/* 6-bit outcode of a point against an AABB. Boundaries are closed-exterior: a
 * coordinate exactly on a face sets that face's bit (matching the binary's
 * p<=min / p>=max tests). */
EXPORT
int axis_aabb_outcode(const float *p, const float *bmin, const float *bmax) {
    int c = 0;
    if (p[0] <= bmin[0]) c |= AXIS_OC_XMIN; else if (p[0] >= bmax[0]) c |= AXIS_OC_XMAX;
    if (p[1] <= bmin[1]) c |= AXIS_OC_YMIN; else if (p[1] >= bmax[1]) c |= AXIS_OC_YMAX;
    if (p[2] <= bmin[2]) c |= AXIS_OC_ZMIN; else if (p[2] >= bmax[2]) c |= AXIS_OC_ZMAX;
    return c;
}

/* Cohen-Sutherland trivial reject: non-zero when the segment a->b is guaranteed
 * to miss the box because both endpoints lie beyond the same face. (Conservative:
 * a 0 return does not by itself prove a hit; see axis_seg_aabb for the test.) */
EXPORT
int axis_seg_aabb_reject(const float *a, const float *b, const float *bmin, const float *bmax) {
    return (axis_aabb_outcode(a, bmin, bmax) & axis_aabb_outcode(b, bmin, bmax)) != 0;
}

/* Definitive segment / AABB intersection by the slab method.
 *   a,b          : segment endpoints
 *   bmin,bmax    : box
 *   tEnter (opt) : set to the entry parameter in [0,1] on a hit (0 if a is inside)
 * Returns 1 if the closed segment touches the closed box, else 0. */
EXPORT
int axis_seg_aabb(const float *a, const float *b, const float *bmin, const float *bmax, float *tEnter) {
    float t0 = 0.0f, t1 = 1.0f;
    for (int i = 0; i < 3; i++) {
        float d = b[i] - a[i];
        if (d > -1e-20f && d < 1e-20f) {
            /* segment parallel to this slab: must already be within it */
            if (a[i] < bmin[i] || a[i] > bmax[i]) return 0;
        } else {
            float inv = 1.0f / d;
            float tn = (bmin[i] - a[i]) * inv;
            float tf = (bmax[i] - a[i]) * inv;
            if (tn > tf) { float tmp = tn; tn = tf; tf = tmp; }
            if (tn > t0) t0 = tn;
            if (tf < t1) t1 = tf;
            if (t0 > t1) return 0;
        }
    }
    if (tEnter) *tEnter = t0;
    return 1;
}
