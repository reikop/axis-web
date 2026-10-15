/* AXIS PTX texture decoder -- reconstructed baseline-JPEG codec (docs/SPEC.md
 * §2.2), ported byte-for-byte from the verified web/ptx.js / tools/ptx_decode.py.
 *
 * A .PTX is a baseline-JPEG variant whose two DC and two AC Huffman tables live
 * inside AXIS.EXE (read at run time -- nothing embedded). decodePTX(ptx, exe)
 * returns RGBA. This is the heaviest asset decoder and the game's own texture
 * logic; compiling it to WASM (approach B) is what lets the client turn the
 * user's raw Image*.dat PTX entries into pixels without a JS fallback.
 *
 * Byte-exactness vs the reference is delicate because the pipeline is floating
 * point (separable IDCT + YCbCr). Two measures make the WASM output bit-identical
 * to the JS/Python reference, which port/wasm/verify_emcc.js checks on real
 * textures: (1) the IDCT basis M is embedded as the EXACT 64-bit patterns the
 * reference computes from cos/sqrt (so no libm last-ULP drift), and (2) the build
 * uses -ffp-contract=off and wasm has no fused multiply-add, so every multiply
 * and add rounds exactly as the reference's double arithmetic does, in the same
 * order. __builtin_floor maps to wasm f64.floor (exact).
 *
 * Sixth reconstructed primitive of the approach-B port; third asset decoder.
 */
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

static inline uint16_t ru16(const uint8_t *p) { uint16_t v; __builtin_memcpy(&v, p, 2); return v; }
static inline uint32_t ru32(const uint8_t *p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
static inline int32_t  ri32(const uint8_t *p) { int32_t v;  __builtin_memcpy(&v, p, 4); return v; }

/* IDCT basis M[x*8+u] = C[u]*cos((2x+1)u*pi/16), exact bits (see scratchpad/gen_m.js). */
static const uint64_t M_BITS[64] = {
    0x3fd6a09e667f3bcdULL, 0x3fdf6297cff75cb0ULL, 0x3fdd906bcf328d46ULL, 0x3fda9b66290ea1a3ULL,
    0x3fd6a09e667f3bcdULL, 0x3fd1c73b39ae68c9ULL, 0x3fc87de2a6aea964ULL, 0x3fb8f8b83c69a60dULL,
    0x3fd6a09e667f3bcdULL, 0x3fda9b66290ea1a3ULL, 0x3fc87de2a6aea964ULL, 0xbfb8f8b83c69a608ULL,
    0xbfd6a09e667f3bccULL, 0xbfdf6297cff75cb0ULL, 0xbfdd906bcf328d47ULL, 0xbfd1c73b39ae68c8ULL,
    0x3fd6a09e667f3bcdULL, 0x3fd1c73b39ae68c9ULL, 0xbfc87de2a6aea962ULL, 0xbfdf6297cff75cb0ULL,
    0xbfd6a09e667f3bceULL, 0x3fb8f8b83c69a60cULL, 0x3fdd906bcf328d44ULL, 0x3fda9b66290ea1a6ULL,
    0x3fd6a09e667f3bcdULL, 0x3fb8f8b83c69a60dULL, 0xbfdd906bcf328d46ULL, 0xbfd1c73b39ae68c8ULL,
    0x3fd6a09e667f3bcbULL, 0x3fda9b66290ea1a6ULL, 0xbfc87de2a6aea965ULL, 0xbfdf6297cff75cb2ULL,
    0x3fd6a09e667f3bcdULL, 0xbfb8f8b83c69a608ULL, 0xbfdd906bcf328d47ULL, 0x3fd1c73b39ae68c5ULL,
    0x3fd6a09e667f3bceULL, 0xbfda9b66290ea1a2ULL, 0xbfc87de2a6aea971ULL, 0x3fdf6297cff75cb0ULL,
    0x3fd6a09e667f3bcdULL, 0xbfd1c73b39ae68c6ULL, 0xbfc87de2a6aea96dULL, 0x3fdf6297cff75cb0ULL,
    0xbfd6a09e667f3bc5ULL, 0xbfb8f8b83c69a602ULL, 0x3fdd906bcf328d46ULL, 0xbfda9b66290ea1a1ULL,
    0x3fd6a09e667f3bcdULL, 0xbfda9b66290ea1a4ULL, 0x3fc87de2a6aea967ULL, 0x3fb8f8b83c69a61dULL,
    0xbfd6a09e667f3bc9ULL, 0x3fdf6297cff75cb2ULL, 0xbfdd906bcf328d43ULL, 0x3fd1c73b39ae68c2ULL,
    0x3fd6a09e667f3bcdULL, 0xbfdf6297cff75cb0ULL, 0x3fdd906bcf328d44ULL, 0xbfda9b66290ea1a2ULL,
    0x3fd6a09e667f3bc4ULL, 0xbfd1c73b39ae68c2ULL, 0x3fc87de2a6aea95fULL, 0xbfb8f8b83c69a616ULL,
};

static const int ZIGZAG[64] = {0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,
    13,6,7,14,21,28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,
    39,46,53,60,61,54,47,55,62,63};
static const int QLUMA[64] = {16,11,10,16,24,40,51,61,12,12,14,19,26,58,60,55,14,13,16,24,40,57,69,56,
    14,17,22,29,51,87,80,62,18,22,37,56,68,109,103,77,24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101,72,92,95,98,112,100,103,99};
static const int QCHROMA[64] = {17,18,24,47,99,99,99,99,18,21,26,66,99,99,99,99,24,26,56,99,99,99,99,99,
    47,66,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99};

/* AXIS.EXE Huffman table VAs (SPEC §2.2): 2 DC (12 entries, stride 0x60),
 * 2 AC (256 entries, stride 0x800). Each entry is (int32 len, int32 code). */
#define DC_VA 0x4c6a98u
#define DC_STRIDE 0x60u
#define AC_VA 0x4c6b58u
#define AC_STRIDE 0x800u

#define MAXWH (1024 * 1024)
static uint8_t PLANE[4][MAXWH];

static uint32_t va_to_off(const uint8_t *exe, uint32_t exeLen, uint32_t va) {
    uint32_t eLfanew = ru32(exe + 0x3c);
    uint16_t nsec = ru16(exe + eLfanew + 6);
    uint16_t opt  = ru16(exe + eLfanew + 20);
    uint32_t base = ru32(exe + eLfanew + 24 + 28);
    uint32_t rva = va - base, sec = eLfanew + 24 + opt;
    for (uint16_t i = 0; i < nsec; i++) {
        uint32_t s = sec + (uint32_t)i * 40;
        uint32_t vs = ru32(exe + s + 8), vaddr = ru32(exe + s + 12), praw = ru32(exe + s + 20);
        uint32_t span = (vs > 1 ? vs : 1) + 0x2000;
        if (rva >= vaddr && rva < vaddr + span) return praw + (rva - vaddr);
    }
    (void)exeLen;
    return 0xffffffffu;
}

/* ext(v,n): sign-extend an n-bit magnitude, JPEG style. */
static inline int ext_(uint32_t v, int n) { return ((int)v >= (1 << (n - 1))) ? (int)v : (int)v - (1 << n) + 1; }

/* round-half-to-even, matching Python round() / web/ptx.js rhe(). */
static inline double rhe_(double x) {
    double f = __builtin_floor(x), d = x - f;
    if (d < 0.5) return f;
    if (d > 0.5) return f + 1.0;
    long long fi = (long long)f;
    return (fi % 2 == 0) ? f : f + 1.0;
}

EXPORT
int axis_ptx_decode(const uint8_t *ptx, uint32_t ptxLen, const uint8_t *exe, uint32_t exeLen,
                    uint8_t *rgbaOut, uint32_t *wh) {
    if (!(ptx[0] == 0x50 && ptx[1] == 0x54 && ptx[2] == 0x58 && ptx[3] == 0)) return -1;  /* "PTX\0" */
    uint32_t w8 = ru16(ptx + 6), h8 = ru16(ptx + 8);
    uint32_t fmt = ru16(ptx + 10), quality = ru16(ptx + 12);
    uint32_t w = w8 * 8, h = h8 * 8;
    int planes = (fmt == 0x82) ? 4 : 3;
    if (w * h > (uint32_t)MAXWH) return -2;

    double M[64];
    for (int i = 0; i < 64; i++) __builtin_memcpy(&M[i], &M_BITS[i], 8);

    /* Huffman tables from the EXE. */
    int dcLen[2][12], dcCode[2][12], acLen[2][256], acCode[2][256];
    for (int t = 0; t < 2; t++) {
        uint32_t off = va_to_off(exe, exeLen, DC_VA + (uint32_t)t * DC_STRIDE);
        if (off == 0xffffffffu) return -3;
        for (int i = 0; i < 12; i++) { dcLen[t][i] = ri32(exe + off + i * 8); dcCode[t][i] = ri32(exe + off + i * 8 + 4); }
    }
    for (int t = 0; t < 2; t++) {
        uint32_t off = va_to_off(exe, exeLen, AC_VA + (uint32_t)t * AC_STRIDE);
        if (off == 0xffffffffu) return -3;
        for (int i = 0; i < 256; i++) { acLen[t][i] = ri32(exe + off + i * 8); acCode[t][i] = ri32(exe + off + i * 8 + 4); }
    }

    /* scaled quant tables */
    int ql[64], qc[64];
    for (int i = 0; i < 64; i++) {
        int a = (int)((double)(QLUMA[i] * 100) / (double)quality);   if (a < 1) a = 1; ql[i] = a;
        int c = (int)((double)(QCHROMA[i] * 100) / (double)quality); if (c < 1) c = 1; qc[i] = c;
    }

    /* MSB-first bit reader over the payload; bytes past the end read as 0 (padding). */
    uint32_t bp = 16 * 8;
    uint32_t totalBits = ptxLen * 8;
    #define BIT(i) (((uint32_t)(((i) >> 3) < ptxLen ? ptx[(i) >> 3] : 0) >> (7 - ((i) & 7))) & 1u)

    uint32_t bx = w >> 3, by = h >> 3;
    double tmp[64], out[64], coef[64];

    for (int p = 0; p < planes; p++) {
        int mode = (p == 1 || p == 2) ? 1 : 0;
        const int *q = mode ? qc : ql;
        const int *dcL = dcLen[mode], *dcC = dcCode[mode], *acL = acLen[mode], *acC = acCode[mode];
        int pred = 0, over = 0;
        uint8_t *px = PLANE[p];
        for (uint32_t i = 0; i < w * h; i++) px[i] = 128;

        for (uint32_t bb = 0; bb < bx * by && !over; bb++) {
            if (bp > totalBits + 16) { over = 1; break; }
            for (int i = 0; i < 64; i++) coef[i] = 0.0;

            /* DC */
            uint32_t pk = 0; for (int i = 0; i < 16; i++) pk = (pk << 1) | BIT(bp + i);
            int diff = 0, matched = 0;
            for (int c = 0; c < 12; c++) {
                int ln = dcL[c];
                if (ln && (pk >> (16 - ln)) == (uint32_t)dcC[c]) {
                    bp += ln;
                    if (c != 0) { uint32_t v = 0; for (int i = 0; i < c; i++) v = (v << 1) | BIT(bp + i); bp += c; diff = ext_(v, c); }
                    matched = 1; break;
                }
            }
            if (!matched) return -4;
            pred += diff; coef[0] = (double)pred;

            /* AC */
            int k = 1;
            while (k < 64) {
                uint32_t apk = 0; for (int i = 0; i < 16; i++) apk = (apk << 1) | BIT(bp + i);
                int run = 0, size = 0, val = 0, m2 = 0;
                for (int s = 0; s < 256; s++) {
                    int ln = acL[s];
                    if (ln && (apk >> (16 - ln)) == (uint32_t)acC[s]) {
                        bp += ln; run = s >> 4; size = s & 15;
                        if (size != 0) { uint32_t v = 0; for (int i = 0; i < size; i++) v = (v << 1) | BIT(bp + i); bp += size; val = ext_(v, size); }
                        m2 = 1; break;
                    }
                }
                if (!m2) return -5;
                if (val == 0 && run == 0) break;
                k += run; if (k >= 64) break;
                coef[ZIGZAG[k]] = (double)val; k++;
            }

            for (int i = 0; i < 64; i++) coef[i] *= (double)q[i];

            /* separable IDCT (row pass then column pass), exact order */
            for (int v = 0; v < 8; v++) {
                int any = 0; for (int u = 0; u < 8; u++) if (coef[v * 8 + u] != 0.0) { any = 1; break; }
                if (!any) { for (int x = 0; x < 8; x++) tmp[v * 8 + x] = 0.0; continue; }
                for (int x = 0; x < 8; x++) { double s = 0.0; for (int u = 0; u < 8; u++) s += coef[v * 8 + u] * M[x * 8 + u]; tmp[v * 8 + x] = s; }
            }
            for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) { double s = 0.0; for (int v = 0; v < 8; v++) s += tmp[v * 8 + x] * M[y * 8 + v]; out[y * 8 + x] = s; }

            uint32_t ox = (bb % bx) * 8, oy = (bb / bx) * 8;
            for (uint32_t yy = 0; yy < 8; yy++) for (uint32_t xx = 0; xx < 8; xx++) {
                int val = (int)rhe_(out[yy * 8 + xx]) + 128;
                if (val < 0) val = 0; else if (val > 255) val = 255;
                px[(oy + yy) * w + (ox + xx)] = (uint8_t)val;
            }
        }
        bp = (bp + 7) & ~7u;
    }

    const uint8_t *Y = PLANE[0], *Cb = PLANE[1], *Cr = PLANE[2], *A = (planes == 4) ? PLANE[3] : 0;
    for (uint32_t i = 0; i < w * h; i++) {
        double y = (double)Y[i], cb = (double)Cb[i] - 128.0, cr = (double)Cr[i] - 128.0;
        double r = y + 1.402 * cr, g = y - 0.344136 * cb - 0.714136 * cr, bl = y + 1.772 * cb;
        uint32_t j = i * 4;
        rgbaOut[j]     = r  < 0 ? 0 : r  > 255 ? 255 : (uint8_t)(int)r;
        rgbaOut[j + 1] = g  < 0 ? 0 : g  > 255 ? 255 : (uint8_t)(int)g;
        rgbaOut[j + 2] = bl < 0 ? 0 : bl > 255 ? 255 : (uint8_t)(int)bl;
        rgbaOut[j + 3] = A ? A[i] : 255;
    }
    wh[0] = w; wh[1] = h;
    return 0;
    #undef BIT
}
