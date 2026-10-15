/* Verify the simulation math primitives (port/sim/axis_vecmath.c) against known
 * mathematical identities — self-contained, no game data needed.
 *   cc axis_vecmath.c axis_vecmath_test.c -lm -o t && ./t
 */
#include <math.h>
#include <stdio.h>

float axis_vec_len(const float *v);
void  axis_vec_cross(const float *a, const float *b, float *out);
float axis_vec_normalize(const float *v, float *out);
void  axis_rotate_axis_angle(const float *v, const float *axis, float angle, float *out);
void  axis_integrate(float *pos, const float *vel, float dt);

static int fails = 0;
static void ck(const char *what, int cond) { if (!cond) { printf("  FAIL %s\n", what); fails++; } }
static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

int main(void) {
    /* length */
    ck("len(3,4,0)=5",  near(axis_vec_len((float[]){3,4,0}), 5.0f));
    ck("len(1,2,2)=3",  near(axis_vec_len((float[]){1,2,2}), 3.0f));

    /* cross product: x × y = z, and result orthogonal to inputs */
    float c[3]; axis_vec_cross((float[]){1,0,0}, (float[]){0,1,0}, c);
    ck("x cross y = z", near(c[0],0) && near(c[1],0) && near(c[2],1));
    float a[3]={1,2,3}, b[3]={-2,0,5}, x[3];
    axis_vec_cross(a, b, x);
    ck("cross ⟂ a", near(x[0]*a[0]+x[1]*a[1]+x[2]*a[2], 0));
    ck("cross ⟂ b", near(x[0]*b[0]+x[1]*b[1]+x[2]*b[2], 0));

    /* normalize: unit length, direction preserved */
    float n[3]; float l = axis_vec_normalize((float[]){0,3,4}, n);
    ck("normalize len recovered", near(l, 5.0f));
    ck("normalize -> unit", near(axis_vec_len(n), 1.0f));

    /* rotation: 90° about z sends x -> y; 2π is identity; length preserved */
    float r[3];
    axis_rotate_axis_angle((float[]){1,0,0}, (float[]){0,0,1}, (float)M_PI/2, r);
    ck("rot x by 90°/z = y", near(r[0],0) && near(r[1],1) && near(r[2],0));
    axis_rotate_axis_angle((float[]){1,0,0}, (float[]){0,0,1}, 0.0f, r);
    ck("rot by 0 = identity", near(r[0],1) && near(r[1],0) && near(r[2],0));
    axis_rotate_axis_angle((float[]){0.3f,-0.7f,0.2f}, (float[]){1,1,1}, 2.0f*(float)M_PI, r);
    ck("rot by 2pi = identity", near(r[0],0.3f) && near(r[1],-0.7f) && near(r[2],0.2f));
    axis_rotate_axis_angle((float[]){2,-1,3}, (float[]){0,1,0}, 1.1f, r);
    ck("rotation preserves length", near(axis_vec_len(r), axis_vec_len((float[]){2,-1,3})));

    /* integrate: pos += vel*dt */
    float p[3]={1,2,3}; axis_integrate(p, (float[]){10,-5,0}, 0.5f);
    ck("integrate", near(p[0],6) && near(p[1],-0.5f) && near(p[2],3));

    printf(fails ? "\nVEC-MATH: %d FAILED\n" : "\nVEC-MATH: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
