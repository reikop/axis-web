/* Verify the animated-prop layer (axis_prop.c) against the identities the
 * reconstruction must satisfy — no game ground-truth required:
 *   - phase advance stays in [0,1]; CLAMP holds at top and stops+notifies at
 *     bottom; ONESHOT resets+notifies at either end; rate zeroed at stop;
 *   - activation radius = (range*100)^2, inside iff dist^2 < radius^2;
 *   - nearest-actor scan ignores inactive actors and returns the minimum;
 *   - glow alpha ramps +/-4/sec clamped to [0,1];
 *   - clip-space visibility is the -w<=x,y,z<=w test.
 *   cc axis_prop.c axis_prop_test.c -lm -o t && ./t
 */
#include <math.h>
#include <stdio.h>

enum { CLAMP = 1, ONESHOT = 3 };

float axis_anim_phase_step(float phase, float *rate, int mode, float dt, int armed, int *callback);
float axis_prop_radius2(int rangeSteps);
int   axis_prop_inside(float dist2, int rangeSteps);
float axis_prop_nearest_dist2(const float *propPos, const float *actors, const int *active, int n);
float axis_glow_fade(float alpha, float dt, int visibleUnoccluded);
int   axis_clip_visible(float x, float y, float z, float w);

static int fails = 0;
static void ck(const char *w, int c) { if (!c) { printf("  FAIL %s\n", w); fails++; } }
static int approx(float a, float b, float eps) { float d = a - b; if (d < 0) d = -d; return d < eps; }

int main(void) {
    int cb;

    /* --- (1) phase advance, CLAMP mode --- */
    {
        float rate = 0.5f;
        float p = axis_anim_phase_step(0.2f, &rate, CLAMP, 0.1f, 0, &cb);  /* +0.05 */
        ck("clamp advances by dt*rate", approx(p, 0.25f, 1e-4f));
        ck("no callback mid-animation", cb == 0);
        ck("rate unchanged mid-animation", approx(rate, 0.5f, 1e-6f));
    }
    {
        float rate = 10.0f;                      /* will overshoot top */
        float p = axis_anim_phase_step(0.95f, &rate, CLAMP, 0.1f, 1, &cb);
        ck("clamp holds at top (phase=1)", approx(p, 1.0f, 1e-6f));
        ck("clamp stops at top (rate=0)", rate == 0.0f);
        ck("no bottom-callback at top", cb == 0);
    }
    {
        float rate = -10.0f;                     /* will undershoot bottom */
        float p = axis_anim_phase_step(0.05f, &rate, CLAMP, 0.1f, 1, &cb);
        ck("clamp closes at bottom (phase=0)", approx(p, 0.0f, 1e-6f));
        ck("clamp stops at bottom (rate=0)", rate == 0.0f);
        ck("armed callback fires at bottom", cb == 1);
    }
    {
        float rate = -10.0f;
        axis_anim_phase_step(0.05f, &rate, CLAMP, 0.1f, 0, &cb);  /* not armed */
        ck("unarmed: no callback at bottom", cb == 0);
    }

    /* --- ONESHOT mode: reset + notify at either end --- */
    {
        float rate = 10.0f;
        float p = axis_anim_phase_step(0.95f, &rate, ONESHOT, 0.1f, 1, &cb);
        ck("oneshot resets to 0 at top", approx(p, 0.0f, 1e-6f));
        ck("oneshot stops at top", rate == 0.0f);
        ck("oneshot notifies at top", cb == 1);
    }

    /* --- (2) activation radius --- */
    {
        ck("radius^2 = (range*100)^2", approx(axis_prop_radius2(3), 300.0f*300.0f, 1e-1f));
        ck("inside just under radius", axis_prop_inside(299.0f*299.0f, 3) == 1);
        ck("outside just over radius", axis_prop_inside(301.0f*301.0f, 3) == 0);
        ck("exactly at radius is not inside", axis_prop_inside(300.0f*300.0f, 3) == 0);
    }

    /* --- nearest-actor scan --- */
    {
        float prop[3] = { 0, 0, 0 };
        float actors[] = { 500,0,0,   100,0,0,   0,0,50 };   /* d2 = 250000, 10000, 2500 */
        int active[] = { 1, 0, 1 };                           /* middle one inactive */
        float best = axis_prop_nearest_dist2(prop, actors, active, 3);
        ck("nearest ignores inactive, picks min active", approx(best, 2500.0f, 1e-1f));
        int none[] = { 0,0,0 };
        ck("no active actor -> huge distance", axis_prop_nearest_dist2(prop, actors, none, 3) > 1e30f);
    }

    /* --- (3) glow fade --- */
    {
        float a = axis_glow_fade(0.0f, 0.1f, 1);
        ck("glow ramps up 4/sec", approx(a, 0.4f, 1e-4f));
        a = axis_glow_fade(0.9f, 0.1f, 1);
        ck("glow clamps at 1", approx(a, 1.0f, 1e-6f));
        a = axis_glow_fade(1.0f, 0.1f, 0);
        ck("glow ramps down when hidden", approx(a, 0.6f, 1e-4f));
        a = axis_glow_fade(0.1f, 0.1f, 0);
        ck("glow clamps at 0", approx(a, 0.0f, 1e-6f));
    }

    /* --- (4) clip-space visibility --- */
    {
        ck("point at centre is visible", axis_clip_visible(0, 0, 0, 1) == 1);
        ck("inside unit cube visible", axis_clip_visible(0.5f, -0.5f, 0.3f, 1) == 1);
        ck("x beyond +w culled", axis_clip_visible(2, 0, 0, 1) == 0);
        ck("y beyond -w culled", axis_clip_visible(0, -2, 0, 1) == 0);
        ck("z beyond +w culled", axis_clip_visible(0, 0, 2, 1) == 0);
    }

    printf(fails ? "\nPROP: %d FAILED\n" : "\nPROP: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
