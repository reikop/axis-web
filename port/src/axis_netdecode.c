/* AXIS type-0 state-packet decoder — reconstructed from the inbound apply
 * dispatcher AXIS.EXE FUN_0048bca0, case 0 (docs/SPEC.md §4).
 *
 * The per-tick state snapshot (64-byte payload) carries a player's quantised
 * world transform. This decodes one packet into a flat struct, applying the
 * engine's exact dequantisation (verified against the replays):
 *     actor    = payload[4] >> 4
 *     x,y,z    = int16(payload[5,7,9]) * 10.0     (world units)
 *     yaw(deg) = payload[0x0c] * 14.0625, wrapped to +/-1800, / 10   (X-Z plane)
 *
 * Second reconstructed primitive of the approach-B port (after axis_crypt.c);
 * compiles to WASM with Emscripten (../build.sh) and is also hand-assembled +
 * verified with no toolchain by ../wasm/build_wasm.js.
 */
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

struct axis_state {
    int32_t actor;
    float x, y, z;
    float yaw;      /* degrees, +/-180, in the X-Z ground plane (Y is up) */
};

EXPORT
void axis_decode_state(const uint8_t *p, struct axis_state *out) {
    out->actor = p[4] >> 4;
    out->x = (float)*(const int16_t *)(p + 5) * 10.0f;
    out->y = (float)*(const int16_t *)(p + 7) * 10.0f;
    out->z = (float)*(const int16_t *)(p + 9) * 10.0f;
    float raw = (float)p[0x0c] * 14.0625f;
    if (raw > 1800.0f) raw -= 3600.0f;
    out->yaw = raw * 0.1f;
}
