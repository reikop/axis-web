/* AXIS obfuscation cipher — reconstructed from AXIS.EXE FUN_0046c060 (decrypting
 * read) and FUN_0046c0b0 (encrypting write).  See docs/SPEC.md §2.3.
 *
 * Per byte the decrypt is:  x = rol8(b ^ 0x59, 1)
 * and the encrypt is its inverse:  b = (ror8(x, 1)) ^ 0x59
 *
 * This is the first reconstructed-to-C piece of the approach-B port: it compiles
 * to WebAssembly with Emscripten (see ../build.sh) so the browser client can
 * decrypt the user's Map .bfd, .ape, Item/Component/Sound.dat in a hot WASM
 * path instead of JS.  The functions operate in place on a caller-provided
 * buffer in linear memory.
 *
 * `skip` leaves the first `skip` bytes untouched (the plaintext 4-byte version
 * prefix on .bfd/.ape; pass 0 for Sound.dat which is encrypted from offset 0).
 */
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

EXPORT
void axis_decrypt(uint8_t *buf, uint32_t len, uint32_t skip) {
    for (uint32_t i = skip; i < len; i++) {
        uint8_t x = (uint8_t)(buf[i] ^ 0x59);
        buf[i] = (uint8_t)((x << 1) | (x >> 7));   /* rol8(x, 1) */
    }
}

EXPORT
void axis_encrypt(uint8_t *buf, uint32_t len, uint32_t skip) {
    for (uint32_t i = skip; i < len; i++) {
        uint8_t x = buf[i];
        x = (uint8_t)((x >> 1) | (x << 7));        /* ror8(x, 1) */
        buf[i] = (uint8_t)(x ^ 0x59);
    }
}
