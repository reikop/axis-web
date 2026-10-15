/* AXIS item / ammo pickup — reconstructed from FUN_0048e220, the per-tick item
 * pickup dispatcher (one of the four the network tick drives). It walks the item
 * entities (the 0x8888-stride entity blocks, active flag = the denormal
 * 0x00000001) and, for each actor within a box reach of an item, adds that item's
 * quantity to the matching inventory slot, capped at the slot's maximum, then
 * consumes the item. The item table is read straight out of the binary's
 * switch(type/10)/switch(type%10) ladder and its "%d <NAME>" strings:
 *
 *   id  0  "1 30MM CELL"          +1    -> 30MM ammo   (slot 0)
 *   id  1  "40 30MM CELLS"        +40   -> 30MM ammo   (slot 0)
 *   id  2  "40 60MM CELLS"        +40   -> 60MM ammo   (slot 1)
 *   id  3  "40 E PACK CELLS"      +40   -> E-PACK      (slot 2)
 *   id  4  "15 MISSILE CELLS"     +15   -> MISSILE     (slot 3)
 *   id  5  "5 MISSILE CELLS"      +5    -> MISSILE     (slot 3)
 *   id 10  "10 ARMOR"             +10   -> ARMOR       (slot 4)
 *   id 11  "25 ARMOR"             +25   -> ARMOR       (slot 4)
 *   id 12  "50 ARMOR"             +50   -> ARMOR       (slot 4)
 *   id 13  "100 ARMOR"            +100  -> ARMOR       (slot 4)
 *   id 14  "100 EXTENDED ARMOR"   +100  -> ARMOR       (slot 4)
 *   id 15  "10 SHIELD"            +10   -> SHIELD      (slot 5)
 *   id 16  "25 SHIELD"            +25   -> SHIELD      (slot 5)
 *   id 17  "50 SHIELD"            +50   -> SHIELD      (slot 5)
 *   id 18  "100 SHIELD"           +100  -> SHIELD      (slot 5)
 *   id 19  "100 EXTENDED SHIELD"  +100  -> SHIELD      (slot 5)
 *   id>=20 powerups (DAMAGE_AMP / CLOAKING_FIELD / OVERED_BOOSTER / WARP_ZONE /
 *          GRAVITY_ARMOR) — instant effects, no counted quantity (slot -1).
 *
 * The proximity test and the capped add are pure and offline-verifiable; the
 * table is decoded game content. Clean original code, not a decompiler
 * transcription. Verified by axis_pickup_test.c.
 */
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

#define AXIS_PICKUP_REACH_SCALE 1.5f   /* FUN_0048e220 multiplies the item radius by 1.5 */

/* inventory slots the pickups feed */
enum { AXIS_INV_30MM = 0, AXIS_INV_60MM = 1, AXIS_INV_EPACK = 2,
       AXIS_INV_MISSILE = 3, AXIS_INV_ARMOR = 4, AXIS_INV_SHIELD = 5, AXIS_INV_NONE = -1 };

/* Whether an actor is close enough to collect an item: a per-axis box test with
 * half-extent itemRadius*1.5 (matching the binary's |delta| < radius*1.5 checks
 * on x, y and z). */
EXPORT
int axis_pickup_in_range(const float *itemPos, const float *actorPos, float itemRadius) {
    float r = itemRadius * AXIS_PICKUP_REACH_SCALE;
    for (int i = 0; i < 3; i++) {
        float d = itemPos[i] - actorPos[i];
        if (d < 0.0f) d = -d;
        if (!(d < r)) return 0;
    }
    return 1;
}

/* Add a pickup quantity to an inventory count, clamped to the slot maximum
 * (the binary's `if (cap < cur+amount) slot = cap`). */
EXPORT
int axis_pickup_add(int cur, int amount, int cap) {
    int v = cur + amount;
    if (v > cap) v = cap;
    return v;
}

/* The quantity an item id grants (0 for powerups, which are instant effects). */
EXPORT
int axis_pickup_amount(int itemType) {
    switch (itemType) {
    case 0:                      return 1;
    case 1: case 2: case 3:      return 40;
    case 4:                      return 15;
    case 5:                      return 5;
    case 10:                     return 10;
    case 11:                     return 25;
    case 12:                     return 50;
    case 13: case 14:            return 100;
    case 15:                     return 10;
    case 16:                     return 25;
    case 17:                     return 50;
    case 18: case 19:            return 100;
    default:                     return 0;   /* powerups / unknown */
    }
}

/* The inventory slot an item id feeds (AXIS_INV_NONE for powerups). */
EXPORT
int axis_pickup_slot(int itemType) {
    switch (itemType) {
    case 0: case 1:                       return AXIS_INV_30MM;
    case 2:                               return AXIS_INV_60MM;
    case 3:                               return AXIS_INV_EPACK;
    case 4: case 5:                       return AXIS_INV_MISSILE;
    case 10: case 11: case 12: case 13: case 14: return AXIS_INV_ARMOR;
    case 15: case 16: case 17: case 18: case 19: return AXIS_INV_SHIELD;
    default:                              return AXIS_INV_NONE;
    }
}

/* One pickup resolution: if the actor is in range, grant the item (capped) and
 * return 1 (item consumed); else 0. *count is the inventory slot to update. */
EXPORT
int axis_pickup_try(const float *itemPos, const float *actorPos, float itemRadius,
                    int itemType, int *count, int cap) {
    if (!axis_pickup_in_range(itemPos, actorPos, itemRadius)) return 0;
    int amt = axis_pickup_amount(itemType);
    if (count && amt > 0) *count = axis_pickup_add(*count, amt, cap);
    return 1;
}
