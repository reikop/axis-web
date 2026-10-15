/* AXIS simulation — 3D vector / rotation math primitives.
 *
 * The per-entity physics (SPEC §5, port/sim/axis_sim.c) is built on a handful of
 * standard 3D math operations the decompiled code calls out to: vector length
 * (FUN_00466c00), cross product, normalise, and axis-angle rotation
 * (FUN_0047ce20 -> FUN_0047c530 build + FUN_0047cd20 apply), used to steer homing
 * projectiles by a limited turn angle.
 *
 * These are CLEAN, independent reimplementations of standard linear algebra
 * (Rodrigues' rotation etc.) — original code, not transcribed from the binary, so
 * the port stays a clean-room reimplementation. Unlike the per-entity physics
 * bodies, these are small and self-contained, so they are also *verifiable*
 * against known mathematical identities (see axis_vecmath_test.c) rather than
 * needing a live match. They compile to WASM via the standard build.
 *
 * NOTE on precision: the original uses x87 80-bit intermediates (float10); WASM
 * has only f32/f64, so results match the original to f32/f64 precision, not
 * bit-for-bit. For a web-only deployment (all peers the same build) that is the
 * correct target — see SPEC §5.
 */
#include <stdint.h>
#include <math.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

/* |v| — vector length (the FUN_00466c00 role). */
EXPORT
float axis_vec_len(const float *v) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/* out = a × b — cross product (as used for the steering basis in FUN_00479750). */
EXPORT
void axis_vec_cross(const float *a, const float *b, float *out) {
    float x = a[1] * b[2] - a[2] * b[1];
    float y = a[2] * b[0] - a[0] * b[2];
    float z = a[0] * b[1] - a[1] * b[0];
    out[0] = x; out[1] = y; out[2] = z;
}

/* out = v / |v| (unchanged if |v| ~ 0). Returns the original length. */
EXPORT
float axis_vec_normalize(const float *v, float *out) {
    float l = axis_vec_len(v);
    if (l > 1e-20f) { out[0] = v[0] / l; out[1] = v[1] / l; out[2] = v[2] / l; }
    else { out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; }
    return l;
}

/* Rotate vector `v` about the given (not-necessarily-unit) axis by `angle`
 * radians — Rodrigues' rotation formula:
 *   v_rot = v cosθ + (k × v) sinθ + k (k·v)(1 − cosθ),  k = axis/|axis|
 * This is the axis-angle rotation FUN_0047ce20 builds and applies to steer a
 * homing projectile's velocity by a capped turn angle (FUN_00479750). */
EXPORT
void axis_rotate_axis_angle(const float *v, const float *axis, float angle, float *out) {
    float k[3]; axis_vec_normalize(axis, k);
    float c = cosf(angle), s = sinf(angle), t = 1.0f - c;
    float kv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];   /* k·v */
    float cx = k[1] * v[2] - k[2] * v[1];                 /* (k×v) */
    float cy = k[2] * v[0] - k[0] * v[2];
    float cz = k[0] * v[1] - k[1] * v[0];
    out[0] = v[0] * c + cx * s + k[0] * kv * t;
    out[1] = v[1] * c + cy * s + k[1] * kv * t;
    out[2] = v[2] * c + cz * s + k[2] * kv * t;
}

/* Integrate a position by a velocity over dt (the common `pos += vel*dt` the
 * entity updates use when stepping between frames). */
EXPORT
void axis_integrate(float *pos, const float *vel, float dt) {
    pos[0] += vel[0] * dt; pos[1] += vel[1] * dt; pos[2] += vel[2] * dt;
}
