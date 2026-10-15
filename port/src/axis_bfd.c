/* AXIS .bfd map decrypt + object-placement decoder — reconstructed from the map
 * format (docs/SPEC.md §2.3 cipher + §2.6 placement), ported byte-for-byte from
 * the verified web/bfd.js / tools/extract_map.py reference.
 *
 * A Map/*.bfd is the obfuscation cipher applied from offset 4 (the 4-byte
 * version prefix is plaintext). Decrypted it is: [u32 version][u32][u32 count]
 * then `count` records of 0xB0 bytes, each a placed object: cp949 name at +0
 * (up to 0x88) and a float32 world position (x,y,z) at +0x8C. A record whose
 * position is non-finite, or whose |x| exceeds 1e7, is clamped to the origin
 * (the same guard the reference applies to stale/garbage slots).
 *
 * Fifth reconstructed primitive of the approach-B port and the second asset
 * decoder (after axis_shp.c): it decrypts in place with the same rol8 cipher as
 * axis_crypt.c, then parses, so the numeric decode is bit-identical to the
 * JS/Python reference. verify_emcc.js runs it on every Map/*.bfd and compares
 * version, counts and every object position. Names are a plain cp949 decode of
 * bytes the cipher check already proves byte-identical, so they need no re-check.
 */
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

static inline uint32_t ru32(const uint8_t *p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
static inline float    rf32(const uint8_t *p) { float v;    __builtin_memcpy(&v, p, 4); return v; }

#define REC 0xB0
#define POS 0x8C

/* A float is finite iff its exponent field isn't all-ones (covers inf and nan). */
static inline int finite_f(float v) { uint32_t b; __builtin_memcpy(&b, &v, 4); return (b & 0x7f800000u) != 0x7f800000u; }
static inline float absf(float v) { uint32_t b; __builtin_memcpy(&b, &v, 4); b &= 0x7fffffffu; float r; __builtin_memcpy(&r, &b, 4); return r; }

/* Decrypt buf[4..len) in place (rol8(b^0x59,1)), then extract placements.
 * posOut gets 3 floats per emitted object; counts := {version, count(header),
 * emitted}. Returns the emitted count. */
EXPORT
int axis_bfd_extract(uint8_t *buf, uint32_t len, float *posOut, uint32_t *counts) {
    for (uint32_t i = 4; i < len; i++) {
        uint8_t x = (uint8_t)(buf[i] ^ 0x59);
        buf[i] = (uint8_t)((x << 1) | (x >> 7));   /* rol8(x,1) */
    }
    uint32_t version = ru32(buf + 0);
    uint32_t count   = (len >= 12) ? ru32(buf + 8) : 0;
    uint32_t emitted = 0, base = 0xC;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t o = base + i * REC;
        if (o + REC > len) break;                  /* mirrors the JS bounds break */
        float x = rf32(buf + o + POS), y = rf32(buf + o + POS + 4), z = rf32(buf + o + POS + 8);
        if (!(finite_f(x) && finite_f(y) && finite_f(z)) || (double)absf(x) > 1e7) { x = y = z = 0.0f; }
        posOut[emitted * 3 + 0] = x;
        posOut[emitted * 3 + 1] = y;
        posOut[emitted * 3 + 2] = z;
        emitted++;
    }
    counts[0] = version;
    counts[1] = count;
    counts[2] = emitted;
    return (int)emitted;
}
