/* AXIS client input gathering — reconstructed from the input-sampling half of the
 * type-1 packet builder FUN_0048d080 (the per-tick "player action" packet, SPEC
 * §4). Each tick the client scans its 16 action channels and builds two 16-bit
 * edge-triggered masks — one for presses, one for releases — consuming (clearing)
 * each channel's latch as it is read, so an event is reported exactly once. The
 * binary's loop:
 *
 *     for (i = 0; i < 16; i++)
 *         if (channel_enabled(i)) {                 // FUN_004a7d60(i) == 1
 *             if (pressLatch[i] == 1)  { pressLatch[i]  = 0; pressMask  |= 1<<i; }
 *             if (releaseLatch[i] == 1){ releaseLatch[i]= 0; releaseMask|= 1<<i; }
 *         }
 *
 * This is the whole of the client-side input->intent processing on the per-tick
 * path: the masks (fire / jump / boost / weapon-select etc.) plus the player's
 * aim and position are then copied into the packet. Note what is NOT here — there
 * is no input->acceleration integration: the actor's position and orientation are
 * *read from* its state block (written by the network type-0 apply + client-side
 * dead-reckoning, SPEC §5), confirming the authoritative movement transfer is not
 * a client per-tick function. This layer is the reconstructable, offline-
 * verifiable client input piece.
 *
 * Clean original code, not a decompiler transcription. Verified by
 * axis_input_test.c against the edge-trigger identities.
 */
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

#define AXIS_INPUT_CHANNELS 16

/* Build the per-tick press/release action bitmasks from the channels' latches.
 *   enabled[n]      : per-channel enabled flag (==1 to consider the channel)
 *   pressLatch[n]   : per-channel press event latch (set by input, cleared here)
 *   releaseLatch[n] : per-channel release event latch (set by input, cleared here)
 *   n               : channel count (16 in AXIS; clamped to 32 so bit i fits)
 * Writes the accumulated 16(/n)-bit masks; a latched event becomes exactly one
 * mask bit and is consumed, so the next tick does not report it again. */
EXPORT
void axis_input_build_masks(const int *enabled, int *pressLatch, int *releaseLatch, int n,
                            uint32_t *pressMask, uint32_t *releaseMask) {
    uint32_t p = 0, r = 0;
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        if (enabled[i] == 1) {
            if (pressLatch[i] == 1)   { pressLatch[i] = 0;   p |= (uint32_t)1 << i; }
            if (releaseLatch[i] == 1) { releaseLatch[i] = 0; r |= (uint32_t)1 << i; }
        }
    }
    *pressMask = p;
    *releaseMask = r;
}

/* Convenience: test whether action channel `ch` fired in a mask. */
EXPORT
int axis_input_mask_has(uint32_t mask, int ch) {
    return (mask & ((uint32_t)1 << ch)) != 0;
}
