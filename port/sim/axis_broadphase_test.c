/* Verify the collision broad-phase layer (axis_broadphase.c) against geometric
 * identities — no game ground-truth required:
 *   - affine transform: identity, pure translation, 90-deg rotation about Y;
 *   - AABB outcode: inside -> 0, each face bit for a point beyond that face,
 *     closed-exterior boundary (on a face -> bit set);
 *   - Cohen-Sutherland trivial reject only fires on true misses;
 *   - slab segment/AABB: crossing hit with correct entry t, inside hit (t=0),
 *     clean miss, parallel-outside miss; reject implies miss (consistency).
 *   cc axis_broadphase.c axis_broadphase_test.c -lm -o t && ./t
 */
#include <math.h>
#include <stdio.h>

#define OC_XMIN 0x000001
#define OC_XMAX 0x000010
#define OC_YMIN 0x000100
#define OC_YMAX 0x001000
#define OC_ZMIN 0x010000
#define OC_ZMAX 0x100000

void axis_xform_affine(float *out, const float *M, const float *T, const float *p);
int  axis_aabb_outcode(const float *p, const float *bmin, const float *bmax);
int  axis_seg_aabb_reject(const float *a, const float *b, const float *bmin, const float *bmax);
int  axis_seg_aabb(const float *a, const float *b, const float *bmin, const float *bmax, float *tEnter);

static int fails = 0;
static void ck(const char *w, int c) { if (!c) { printf("  FAIL %s\n", w); fails++; } }
static int approx(float a, float b, float eps) { float d = a - b; if (d < 0) d = -d; return d < eps; }

int main(void) {
    /* --- affine transform --- */
    {
        float I[9] = { 1,0,0, 0,1,0, 0,0,1 }, T0[3] = { 0,0,0 }, p[3] = { 3,-4,5 }, o[3];
        axis_xform_affine(o, I, T0, p);
        ck("identity transform", approx(o[0],3,1e-5f) && approx(o[1],-4,1e-5f) && approx(o[2],5,1e-5f));
        float T1[3] = { 10,20,30 };
        axis_xform_affine(o, I, T1, p);
        ck("pure translation", approx(o[0],13,1e-5f) && approx(o[1],16,1e-5f) && approx(o[2],35,1e-5f));
        /* +90 deg about Y: (x,y,z) -> (z,y,-x) */
        float Ry[9] = { 0,0,1,  0,1,0,  -1,0,0 };
        float px[3] = { 1,0,0 };
        axis_xform_affine(o, Ry, T0, px);
        ck("rotate +90 about Y maps +X to -Z", approx(o[0],0,1e-5f) && approx(o[1],0,1e-5f) && approx(o[2],-1,1e-5f));
    }

    /* --- outcode --- */
    {
        float bmin[3] = { -1,-1,-1 }, bmax[3] = { 1,1,1 };
        float inside[3] = { 0,0,0 };
        ck("inside -> outcode 0", axis_aabb_outcode(inside, bmin, bmax) == 0);
        float xlo[3] = { -2,0,0 }; ck("x below min bit", axis_aabb_outcode(xlo, bmin, bmax) == OC_XMIN);
        float xhi[3] = {  2,0,0 }; ck("x above max bit", axis_aabb_outcode(xhi, bmin, bmax) == OC_XMAX);
        float yhi[3] = { 0, 2,0 }; ck("y above max bit", axis_aabb_outcode(yhi, bmin, bmax) == OC_YMAX);
        float zlo[3] = { 0,0,-2 }; ck("z below min bit", axis_aabb_outcode(zlo, bmin, bmax) == OC_ZMIN);
        float corner[3] = { -2,2,0 }; ck("corner combines bits", axis_aabb_outcode(corner, bmin, bmax) == (OC_XMIN|OC_YMAX));
        float onface[3] = { 1,0,0 }; ck("point on max face counts outside", (axis_aabb_outcode(onface, bmin, bmax) & OC_XMAX) != 0);
    }

    /* --- trivial reject --- */
    {
        float bmin[3] = { -1,-1,-1 }, bmax[3] = { 1,1,1 };
        float a[3] = { -5,0,0 }, b[3] = { -3,0,0 };   /* both left of box */
        ck("segment wholly left -> reject", axis_seg_aabb_reject(a, b, bmin, bmax) == 1);
        float c[3] = { -5,0,0 }, d[3] = { 5,0,0 };    /* crosses box */
        ck("crossing segment not trivially rejected", axis_seg_aabb_reject(c, d, bmin, bmax) == 0);
    }

    /* --- slab intersection --- */
    {
        float bmin[3] = { -1,-1,-1 }, bmax[3] = { 1,1,1 }, t;
        float a[3] = { -3,0,0 }, b[3] = { 3,0,0 };    /* enters at x=-1 => t=(-1-(-3))/6 */
        ck("crossing hit", axis_seg_aabb(a, b, bmin, bmax, &t) == 1);
        ck("entry t correct", approx(t, 2.0f/6.0f, 1e-5f));
        float ina[3] = { 0,0,0 }, inb[3] = { 0.5f,0,0 };
        ck("inside segment hit (t=0)", axis_seg_aabb(ina, inb, bmin, bmax, &t) == 1 && approx(t,0,1e-5f));
        float ma[3] = { -3,3,0 }, mb[3] = { 3,3,0 };  /* passes above the box */
        ck("miss above box", axis_seg_aabb(ma, mb, bmin, bmax, &t) == 0);
        float pa[3] = { 5,0,0 }, pb[3] = { 5,2,0 };   /* parallel, outside */
        ck("parallel outside miss", axis_seg_aabb(pa, pb, bmin, bmax, &t) == 0);
    }

    /* --- consistency: trivial reject implies the slab test also misses --- */
    {
        float bmin[3] = { -1,-1,-1 }, bmax[3] = { 1,1,1 };
        int bad = 0;
        for (int i = 0; i < 20000; i++) {
            /* cheap deterministic pseudo-points spread around the box */
            float a[3], b[3];
            for (int k = 0; k < 3; k++) {
                a[k] = (float)(((i*7 + k*13) % 11) - 5);
                b[k] = (float)(((i*5 + k*17) % 11) - 5);
            }
            if (axis_seg_aabb_reject(a, b, bmin, bmax) && axis_seg_aabb(a, b, bmin, bmax, 0))
                bad++;
        }
        ck("trivial reject never contradicts a slab hit", bad == 0);
    }

    printf(fails ? "\nBROADPHASE: %d FAILED\n" : "\nBROADPHASE: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
