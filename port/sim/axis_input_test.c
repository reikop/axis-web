/* Verify the client input layer (axis_input.c) against the edge-trigger
 * identities — no game ground-truth required:
 *   - a latched press/release on an enabled channel becomes exactly one mask bit;
 *   - the latch is consumed (cleared), so the next tick reports nothing;
 *   - press and release are independent;
 *   - a disabled channel is ignored and its latch left untouched;
 *   - bit positions match the channel index.
 *   cc axis_input.c axis_input_test.c -lm -o t && ./t
 */
#include <stdio.h>
#include <stdint.h>

void axis_input_build_masks(const int *enabled, int *pressLatch, int *releaseLatch, int n,
                            uint32_t *pressMask, uint32_t *releaseMask);
int  axis_input_mask_has(uint32_t mask, int ch);

static int fails = 0;
static void ck(const char *w, int c) { if (!c) { printf("  FAIL %s\n", w); fails++; } }

int main(void) {
    enum { N = 16 };
    int enabled[N], press[N], rel[N];
    uint32_t pm, rm;

    for (int i = 0; i < N; i++) { enabled[i] = 1; press[i] = 0; rel[i] = 0; }

    /* press on channels 0, 3, 15 -> bits 0,3,15 in press mask */
    press[0] = 1; press[3] = 1; press[15] = 1;
    rel[7] = 1;                               /* release on channel 7 */
    axis_input_build_masks(enabled, press, rel, N, &pm, &rm);
    ck("press bits set for 0,3,15", axis_input_mask_has(pm,0) && axis_input_mask_has(pm,3) && axis_input_mask_has(pm,15));
    ck("press bit not set for others", !axis_input_mask_has(pm,1) && !axis_input_mask_has(pm,7));
    ck("release bit set for 7", axis_input_mask_has(rm,7));
    ck("release not set where only pressed", !axis_input_mask_has(rm,0));
    ck("exact press mask", pm == ((1u<<0)|(1u<<3)|(1u<<15)));
    ck("exact release mask", rm == (1u<<7));

    /* latches consumed: a second build with no new events yields nothing */
    axis_input_build_masks(enabled, press, rel, N, &pm, &rm);
    ck("edge consumed -> empty masks next tick", pm == 0 && rm == 0);
    ck("press latches cleared", press[0]==0 && press[3]==0 && press[15]==0);
    ck("release latch cleared", rel[7]==0);

    /* disabled channel is ignored and its latch preserved */
    for (int i = 0; i < N; i++) { enabled[i] = 0; press[i] = 0; rel[i] = 0; }
    enabled[5] = 0; press[5] = 1;             /* channel 5 disabled but latched */
    enabled[6] = 1; press[6] = 1;             /* channel 6 enabled + latched */
    axis_input_build_masks(enabled, press, rel, N, &pm, &rm);
    ck("disabled channel not reported", !axis_input_mask_has(pm,5));
    ck("disabled channel latch preserved", press[5] == 1);
    ck("enabled channel reported", axis_input_mask_has(pm,6));
    ck("enabled channel latch cleared", press[6] == 0);

    printf(fails ? "\nINPUT: %d FAILED\n" : "\nINPUT: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
