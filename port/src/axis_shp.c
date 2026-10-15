/* AXIS .shp mesh geometry decoder — reconstructed from the Shape.dat record
 * layout (docs/SPEC.md §2.1 archive + §2.8 mesh), ported byte-for-byte from the
 * verified web/shp.js / tools/parse_shp.py reference.
 *
 * A Shape.dat record is one mesh object: header 0x4C (groupCount @ +0x10,
 * tag 0x71110300 @ +0x30), then groupCount group headers of 0x80 bytes
 * (vertexCount @ +0x34), then per group a packed block of
 *   vc*pos(12) + vc*norm(12) + vc*attr(4) + vc*uv(8) + an index region of
 *   triangles (three u32, each < vc) interleaved with 12-byte descriptors whose
 *   3rd word is a stale saved pointer (0x04000000..0x40000000, skipped).
 * Group vertices are concatenated; triangle indices are made global (+base).
 *
 * This is the fourth reconstructed primitive of the approach-B port and the
 * first that is a real asset *decoder* (after the cipher / state codec / RNG):
 * it is pure parse+copy — no arithmetic — so the WASM output is bit-identical to
 * the JS/Python reference, which port/wasm/verify_emcc.js checks on every mesh
 * in the user's Shape.dat. Compiles into the standalone reactor via ../build.sh.
 *
 * Geometry only; the per-group texture index string (group+4, "01".."NN") is
 * metadata the existing name reader handles and is not needed for geometry.
 */
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

static inline float    rf32(const uint8_t *p) { float v;    __builtin_memcpy(&v, p, 4); return v; }
static inline uint32_t ru32(const uint8_t *p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
static inline int32_t  ri32(const uint8_t *p) { int32_t v;  __builtin_memcpy(&v, p, 4); return v; }

#define OBJ_HDR 0x4c
#define GRP_HDR 0x80
#define TAG     0x71110300u

/* Parse one mesh record buf[off..end). Writes concatenated geometry into the
 * caller's output buffers and per-group [triStart,triEnd] into grpOut (2 u32
 * per group). counts := {vtotal, ttotal, gcount, consumed}. Returns 0, or -1 if
 * the tag is wrong (mirrors shp.js throwing). Caller sizes the output buffers;
 * the verifier uses generous fixed regions.
 */
EXPORT
int axis_shp_parse(const uint8_t *buf, uint32_t off, uint32_t end,
                   float *posOut, float *nrmOut, float *uvOut,
                   uint32_t *triOut, uint32_t *grpOut, uint32_t *counts) {
    if (ru32(buf + off + 0x30) != TAG) { counts[0] = counts[1] = counts[2] = counts[3] = 0; return -1; }
    int32_t gcount = ri32(buf + off + 0x10);

    uint32_t vtotal = 0, ttotal = 0;
    uint32_t p = off + OBJ_HDR + (uint32_t)gcount * GRP_HDR;

    for (int32_t g = 0; g < gcount; g++) {
        uint32_t gh = off + OBJ_HDR + (uint32_t)g * GRP_HDR;
        int32_t vc = ri32(buf + gh + 0x34);
        uint32_t base = vtotal;            /* global index offset for this group */
        uint32_t triStart = ttotal;

        for (int32_t i = 0; i < vc; i++) {
            const uint8_t *s = buf + p + (uint32_t)i * 12;
            posOut[(vtotal + i) * 3 + 0] = rf32(s);
            posOut[(vtotal + i) * 3 + 1] = rf32(s + 4);
            posOut[(vtotal + i) * 3 + 2] = rf32(s + 8);
        }
        p += (uint32_t)vc * 12;
        for (int32_t i = 0; i < vc; i++) {
            const uint8_t *s = buf + p + (uint32_t)i * 12;
            nrmOut[(vtotal + i) * 3 + 0] = rf32(s);
            nrmOut[(vtotal + i) * 3 + 1] = rf32(s + 4);
            nrmOut[(vtotal + i) * 3 + 2] = rf32(s + 8);
        }
        p += (uint32_t)vc * 12;
        p += (uint32_t)vc * 4;             /* attr (skipped) */
        for (int32_t i = 0; i < vc; i++) {
            const uint8_t *s = buf + p + (uint32_t)i * 8;
            uvOut[(vtotal + i) * 2 + 0] = rf32(s);
            uvOut[(vtotal + i) * 2 + 1] = rf32(s + 4);
        }
        p += (uint32_t)vc * 8;

        /* index region: triangles (3x u32, each < vc) interleaved with 12-byte
         * descriptors whose 3rd word is a stale pointer (0x04000000..0x40000000). */
        while (p + 12 <= end) {
            uint32_t a = ru32(buf + p), b = ru32(buf + p + 4), c = ru32(buf + p + 8);
            if (a < (uint32_t)vc && b < (uint32_t)vc && c < (uint32_t)vc) {
                triOut[ttotal * 3 + 0] = a + base;
                triOut[ttotal * 3 + 1] = b + base;
                triOut[ttotal * 3 + 2] = c + base;
                ttotal++;
                p += 12;
            } else if (c > 0x04000000u && c < 0x40000000u) {
                p += 12;
            } else {
                break;
            }
        }

        grpOut[g * 2 + 0] = triStart;
        grpOut[g * 2 + 1] = ttotal;        /* triEnd */
        vtotal += (uint32_t)vc;
    }

    counts[0] = vtotal;
    counts[1] = ttotal;
    counts[2] = (uint32_t)gcount;
    counts[3] = p - off;                   /* consumed */
    return 0;
}
