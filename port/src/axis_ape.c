/* AXIS .ape replay decoder -- reconstructed from the replay format (docs/SPEC.md
 * §4), ported byte-for-byte from the verified web/ape.js / tools/parse_ape.py.
 *
 * A replay is the obfuscation cipher applied to the whole file; if that doesn't
 * yield the 0x1201 version dword the first 4 bytes are kept plaintext. The body
 * is 76-byte records [u32 tick][u32][u32 0x40][64-byte payload]; this walks them
 * and extracts the per-actor world-position trajectories from the type-0 state
 * snapshots (actor = payload[4]>>4; x,y,z = int16(payload[5,7,9])*10; yaw =
 * payload[0xc]*14.0625 wrapped to +/-1800, /10). Record-order output.
 *
 * Eighth reconstructed primitive of the approach-B port; fifth asset decoder.
 * The integer fields are pure copy/scale (bit-identical); yaw is computed in the
 * reference's float64, matched here with double arithmetic. Player names (type-2)
 * and type tallies are a plain decode of the same decrypted bytes (the cipher is
 * already verified byte-identical) and stay in the thin JS/render layer.
 */
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

static inline uint32_t ru32(const uint8_t *p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
static inline int16_t  ri16(const uint8_t *p) { int16_t v;  __builtin_memcpy(&v, p, 2); return v; }

#define REC 76
#define PAY 12

/* Decrypt buf in place, find the record stream, and emit type-0 trajectory rows.
 * rowOut: 5 int32 per type-0 record {actor, tick, x, y, z}; yawOut: 1 double per
 * type-0 record. counts := {start (0xffffffff if not found), records, type0 rows}.
 * Returns the record count. */
EXPORT
int axis_ape_parse(uint8_t *buf, uint32_t len, int32_t *rowOut, double *yawOut, uint32_t *counts) {
    /* conditional decrypt: whole buffer, keep plaintext first 4 bytes unless the
     * full decrypt already produced the 0x1201 version dword. */
    uint8_t b0 = buf[0], b1 = buf[1], b2 = buf[2], b3 = buf[3];
    for (uint32_t i = 0; i < len; i++) {
        uint8_t x = (uint8_t)(buf[i] ^ 0x59);
        buf[i] = (uint8_t)((x << 1) | (x >> 7));
    }
    if (!(len >= 4 && ru32(buf) == 0x1201u)) {
        if (len > 0) buf[0] = b0; if (len > 1) buf[1] = b1; if (len > 2) buf[2] = b2; if (len > 3) buf[3] = b3;
    }

    /* findStart: first offset in [0,4096) with 3 consecutive 0x40-tagged records. */
    int32_t start = -1;
    uint32_t lim = len < 4096 ? len : 4096;
    for (uint32_t o = 0; o < lim; o++) {
        int ok = 1;
        for (int k = 0; k < 3; k++) {
            uint32_t b = o + (uint32_t)k * REC;
            if (b + REC > len || ru32(buf + b + 8) != 0x40u) { ok = 0; break; }
        }
        if (ok) { start = (int32_t)o; break; }
    }
    if (start < 0) { counts[0] = 0xffffffffu; counts[1] = 0; counts[2] = 0; return 0; }

    uint32_t records = 0, rows = 0, o = (uint32_t)start;
    while (o + REC <= len) {
        if (ru32(buf + o + 8) != 0x40u) break;
        uint32_t tick = ru32(buf + o);
        const uint8_t *p = buf + o + PAY;
        records++;
        if (p[1] == 0) {
            int32_t actor = p[4] >> 4;
            int32_t x = (int32_t)ri16(p + 5) * 10, y = (int32_t)ri16(p + 7) * 10, z = (int32_t)ri16(p + 9) * 10;
            double yaw = (double)p[0x0c] * 14.0625; if (yaw > 1800.0) yaw -= 3600.0; yaw /= 10.0;
            rowOut[rows * 5 + 0] = actor;
            rowOut[rows * 5 + 1] = (int32_t)tick;
            rowOut[rows * 5 + 2] = x;
            rowOut[rows * 5 + 3] = y;
            rowOut[rows * 5 + 4] = z;
            yawOut[rows] = yaw;
            rows++;
        }
        o += REC;
    }
    counts[0] = (uint32_t)start;
    counts[1] = records;
    counts[2] = rows;
    return (int)records;
}
