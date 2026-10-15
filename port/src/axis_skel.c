/* AXIS Skeleton.dat bone-record decoder -- reconstructed from the rig format
 * (docs/SPEC.md §2.10), ported byte-for-byte from the verified web/skel.js /
 * tools/parse_skeleton.py.
 *
 * A Skeleton.dat entry is one rig: boneCount (i32 @ +0x10) then boneCount bone
 * records of 0x70 bytes starting at +0x24, each holding a cp949 name (+0),
 * parent index (i32 @ +0x40), a translation (f32[3] @ +0x44) and an axis-angle
 * rotation (angle f32 @ +0x50, unit axis f32[3] @ +0x54). This decodes those
 * records; it is pure parse+copy, so the WASM output is bit-identical to the
 * reference, which verify_emcc.js checks on every rig.
 *
 * Only the decode lives here. Building bind-pose world joints (web/skel.js
 * worldJoints) needs cos/sin/hypot on per-bone angles -- transcendental math
 * whose last-ULP result can't be guaranteed to match the JS reference across
 * libraries -- so that stays in the render layer, not this byte-exact decoder.
 *
 * Seventh reconstructed primitive of the approach-B port; fourth asset decoder.
 */
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

static inline int32_t ri32(const uint8_t *p) { int32_t v; __builtin_memcpy(&v, p, 4); return v; }
static inline float   rf32(const uint8_t *p) { float v;   __builtin_memcpy(&v, p, 4); return v; }

#define STRIDE 0x70
#define FIRST  0x24

/* Parse rig buf[off..off+size). parentOut[b] = parent index; fOut[b*7+0..2] =
 * translation, fOut[b*7+3] = angle, fOut[b*7+4..6] = axis. counts := {boneCount
 * (header), emitted}. Returns emitted count. */
EXPORT
int axis_skel_parse(const uint8_t *buf, uint32_t off, uint32_t size,
                    int32_t *parentOut, float *fOut, uint32_t *counts) {
    int32_t boneCount = ri32(buf + off + 0x10);
    uint32_t emitted = 0;
    for (int32_t b = 0; b < boneCount; b++) {
        uint32_t bo = FIRST + (uint32_t)b * STRIDE;
        if (bo + STRIDE > size) break;                 /* mirrors the JS bounds break */
        const uint8_t *r = buf + off + bo;
        parentOut[emitted] = ri32(r + 0x40);
        fOut[emitted * 7 + 0] = rf32(r + 0x44);
        fOut[emitted * 7 + 1] = rf32(r + 0x48);
        fOut[emitted * 7 + 2] = rf32(r + 0x4c);
        fOut[emitted * 7 + 3] = rf32(r + 0x50);         /* angle */
        fOut[emitted * 7 + 4] = rf32(r + 0x54);
        fOut[emitted * 7 + 5] = rf32(r + 0x58);
        fOut[emitted * 7 + 6] = rf32(r + 0x5c);
        emitted++;
    }
    counts[0] = (uint32_t)boneCount;
    counts[1] = emitted;
    return (int)emitted;
}
