/* Verify the item-pickup layer (axis_pickup.c) against the identities the
 * reconstruction must satisfy — no game ground-truth required:
 *   - box proximity: in range at the item, just inside reach*1.5 on an axis,
 *     out just beyond it, and out if any single axis is beyond;
 *   - capped add: below cap sums, over cap clamps, exactly cap stays;
 *   - item table amounts and slots match the decoded switch ladder;
 *   - axis_pickup_try grants only in range and respects the cap.
 *   cc axis_pickup.c axis_pickup_test.c -lm -o t && ./t
 */
#include <stdio.h>

enum { INV_30MM=0, INV_60MM=1, INV_EPACK=2, INV_MISSILE=3, INV_ARMOR=4, INV_SHIELD=5, INV_NONE=-1 };

int axis_pickup_in_range(const float *itemPos, const float *actorPos, float itemRadius);
int axis_pickup_add(int cur, int amount, int cap);
int axis_pickup_amount(int itemType);
int axis_pickup_slot(int itemType);
int axis_pickup_try(const float *itemPos, const float *actorPos, float itemRadius,
                    int itemType, int *count, int cap);

static int fails = 0;
static void ck(const char *w, int c) { if (!c) { printf("  FAIL %s\n", w); fails++; } }

int main(void) {
    /* --- proximity box (reach = radius*1.5) --- */
    {
        float item[3] = { 100, 50, -20 };
        float at[3]   = { 100, 50, -20 };
        ck("in range at the item", axis_pickup_in_range(item, at, 10.0f) == 1);
        float near[3] = { 100 + 14.9f, 50, -20 };    /* dx=14.9 < 15 (=10*1.5) */
        ck("in range just inside reach", axis_pickup_in_range(item, near, 10.0f) == 1);
        float far[3]  = { 100 + 15.1f, 50, -20 };     /* dx=15.1 > 15 */
        ck("out of range just beyond reach", axis_pickup_in_range(item, far, 10.0f) == 0);
        float oneAxis[3] = { 100, 50 + 20.0f, -20 };  /* y beyond reach */
        ck("out if any single axis beyond", axis_pickup_in_range(item, oneAxis, 10.0f) == 0);
    }

    /* --- capped add --- */
    ck("add below cap sums", axis_pickup_add(100, 40, 999) == 140);
    ck("add over cap clamps", axis_pickup_add(980, 40, 999) == 999);
    ck("add exactly to cap", axis_pickup_add(959, 40, 999) == 999);
    ck("already at cap stays", axis_pickup_add(999, 40, 999) == 999);

    /* --- item table amounts --- */
    ck("id0 = 1 30mm cell", axis_pickup_amount(0) == 1);
    ck("id1 = 40 30mm", axis_pickup_amount(1) == 40);
    ck("id2 = 40 60mm", axis_pickup_amount(2) == 40);
    ck("id3 = 40 epack", axis_pickup_amount(3) == 40);
    ck("id4 = 15 missile", axis_pickup_amount(4) == 15);
    ck("id5 = 5 missile", axis_pickup_amount(5) == 5);
    ck("id10 = 10 armor", axis_pickup_amount(10) == 10);
    ck("id11 = 25 armor", axis_pickup_amount(11) == 25);
    ck("id12 = 50 armor", axis_pickup_amount(12) == 50);
    ck("id13 = 100 armor", axis_pickup_amount(13) == 100);
    ck("id15 = 10 shield", axis_pickup_amount(15) == 10);
    ck("id18 = 100 shield", axis_pickup_amount(18) == 100);
    ck("powerup id20 = no count", axis_pickup_amount(20) == 0);

    /* --- item table slots --- */
    ck("id0/1 -> 30mm slot", axis_pickup_slot(0) == INV_30MM && axis_pickup_slot(1) == INV_30MM);
    ck("id2 -> 60mm slot", axis_pickup_slot(2) == INV_60MM);
    ck("id3 -> epack slot", axis_pickup_slot(3) == INV_EPACK);
    ck("id4/5 -> missile slot", axis_pickup_slot(4) == INV_MISSILE && axis_pickup_slot(5) == INV_MISSILE);
    ck("id10..14 -> armor slot", axis_pickup_slot(10) == INV_ARMOR && axis_pickup_slot(14) == INV_ARMOR);
    ck("id15..19 -> shield slot", axis_pickup_slot(15) == INV_SHIELD && axis_pickup_slot(19) == INV_SHIELD);
    ck("powerup -> no slot", axis_pickup_slot(20) == INV_NONE);

    /* --- try: in range grants + caps; out of range does nothing --- */
    {
        float item[3] = { 0,0,0 }, at[3] = { 0,0,0 }, far[3] = { 1000,0,0 };
        int c = 950, got;
        got = axis_pickup_try(item, at, 10.0f, 1 /*+40*/, &c, 999);
        ck("try in range consumes item", got == 1);
        ck("try grants capped amount", c == 990);
        int c2 = 0, got2 = axis_pickup_try(item, far, 10.0f, 1, &c2, 999);
        ck("try out of range does nothing", got2 == 0 && c2 == 0);
    }

    printf(fails ? "\nPICKUP: %d FAILED\n" : "\nPICKUP: ALL PASS\n", fails);
    return fails ? 1 : 0;
}
