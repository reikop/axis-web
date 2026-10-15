/* AXIS .ape replay decoder, in the browser engine.
 *
 * JS port of tools/parse_ape.py (see docs/SPEC.md §4). Decrypts a replay and
 * extracts per-actor world-position trajectories (type-0 state snapshots) plus
 * player names (type-2 identity packets), so the client can play a recorded
 * match straight from the user's own AutoPlay/*.ape in the browser.
 *
 *   parseReplay(ab) -> { records, types, tracks:{actor:[[tick,x,y,z]...]}, names:{actor:str} }
 */
(function (root) {
  'use strict';
  const dec = (typeof TextDecoder !== 'undefined') ? new TextDecoder('euc-kr') : null;
  const REC = 76, PAY = 12;

  function decByte(x) { x = (x ^ 0x59) & 0xFF; return ((x << 1) | (x >> 7)) & 0xFF; }

  function decrypt(ab) {
    const src = new Uint8Array(ab), out = new Uint8Array(src.length);
    for (let i = 0; i < src.length; i++) out[i] = decByte(src[i]);
    // version prefix 0x1201 stays plaintext when the whole-buffer decrypt doesn't yield it
    const dv = new DataView(out.buffer);
    if (out.length >= 4 && dv.getUint32(0, true) === 0x1201) return out;
    for (let i = 0; i < 4 && i < src.length; i++) out[i] = src[i];
    return out;
  }

  function findStart(u8, dv) {
    const lim = Math.min(u8.length, 4096);
    for (let o = 0; o < lim; o++) {
      let ok = true;
      for (let k = 0; k < 3; k++) {
        const b = o + k * REC;
        if (b + REC > u8.length || dv.getUint32(b + 8, true) !== 0x40) { ok = false; break; }
      }
      if (ok) return o;
    }
    return -1;
  }

  function name(u8, o, max) {
    let n = 0; while (n < max && u8[o + n]) n++;
    if (dec) return dec.decode(u8.subarray(o, o + n));
    let s = ''; for (let i = 0; i < n; i++) s += String.fromCharCode(u8[o + i]); return s;
  }

  function parseReplay(ab) {
    const u8 = decrypt(ab);
    const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
    const start = findStart(u8, dv);
    const types = {}, tracks = {}, names = {};
    let records = 0;
    if (start < 0) return { records, types, tracks, names, start };
    let o = start;
    while (o + REC <= u8.length) {
      if (dv.getUint32(o + 8, true) !== 0x40) break;
      const tick = dv.getUint32(o, true);
      const p = o + PAY;
      records++;
      const t = u8[p + 1];
      types[t] = (types[t] || 0) + 1;
      if (t === 0) {
        const actor = u8[p + 4] >> 4;
        // int16 world coords; the engine's apply side (FUN_0048bca0) scales by 10
        const x = dv.getInt16(p + 5, true) * 10, y = dv.getInt16(p + 7, true) * 10, z = dv.getInt16(p + 9, true) * 10;
        (tracks[actor] || (tracks[actor] = [])).push([tick, x, y, z]);
      } else if (t === 2) {
        const nm = name(u8, p + 2, 17);
        const a = u8[p + 22] < 16 ? u8[p + 22] : 0;
        if (nm) names[a] = nm;
      }
      o += REC;
    }
    return { records, start, types, tracks, names };
  }

  root.AXISReplay = { decrypt, parseReplay };
  if (typeof module !== 'undefined' && module.exports) module.exports = { decrypt, parseReplay };
})(typeof window !== 'undefined' ? window : globalThis);
