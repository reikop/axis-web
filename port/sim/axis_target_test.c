/* Verify homing target acquisition (port/sim/axis_target.c): picks the nearest
 * eligible actor, skipping the owner, inactive actors, and out-of-range ones.
 *   cc axis_target.c axis_target_test.c -o t && ./t
 */
#include <stdio.h>

int axis_acquire_target(const float *pos, const float *actors, const int *active,
                        int n, int owner, float maxRange);

static int fails = 0;
static void ck(const char *w, int c) { if (!c) { printf("  FAIL %s\n", w); fails++; } }

int main(void) {
    /* actors: 0 far, 1 near, 2 nearest-but-owner, 3 nearest-but-inactive */
    float A[] = { 500,0,0,   100,0,0,   10,0,0,   5,0,0 };
    int act[] = { 1,         1,         1,        0       };
    float pos[3] = {0,0,0};

    ck("nearest eligible = actor 1", axis_acquire_target(pos, A, act, 4, 2, 0) == 1);
    /* with no owner excluded, actor 2 (dist 10) is nearest eligible (3 inactive) */
    ck("no owner -> actor 2", axis_acquire_target(pos, A, act, 4, -1, 0) == 2);
    /* range cap 50 excludes actor 1 (100); nearest eligible in range is actor 2 (10) */
    ck("range cap picks in-range", axis_acquire_target(pos, A, act, 4, -1, 50) == 2);
    /* range cap 3 excludes everyone -> -1 */
    ck("none in range -> -1", axis_acquire_target(pos, A, act, 4, -1, 3) == -1);
    /* all inactive -> -1 */
    int none[] = {0,0,0,0};
    ck("no active -> -1", axis_acquire_target(pos, A, none, 4, -1, 0) == -1);

    printf(fails ? "\nTARGET: %d FAILED\n" : "\nTARGET: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
