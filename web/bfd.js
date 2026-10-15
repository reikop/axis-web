/* AXIS .bfd map decrypt + object-placement reader, in the browser engine.
 *
 * JS port of tools/bfd_crypt.py + tools/extract_map.py (see docs/SPEC.md §2.3/§2.6).
 * Lets the client load a map straight from the user's own Map/*.bfd in the browser.
 *
 *   decryptBFD(ab)  -> Uint8Array  (4-byte plaintext version kept, rest rol8(b^0x59,1))
 *   extractMap(ab)  -> { version, count, objects: [{name, pos:[x,y,z]}] }
 */
(function (root) {
  'use strict';
  const dec = (typeof TextDecoder !== 'undefined') ? new TextDecoder('euc-kr') : null;
  const REC = 0xB0, NAMELEN = 0x88, POS = 0x8C;

  function decByte(x) { x = (x ^ 0x59) & 0xFF; return ((x << 1) | (x >> 7)) & 0xFF; }

  function decryptBFD(ab, skip) {
    skip = skip == null ? 4 : skip;
    const src = new Uint8Array(ab), out = new Uint8Array(src.length);
    for (let i = 0; i < skip; i++) out[i] = src[i];
    for (let i = skip; i < src.length; i++) out[i] = decByte(src[i]);
    return out;
  }

  function name(u8, o) {
    let n = 0; while (n < NAMELEN && u8[o + n]) n++;
    if (dec) return dec.decode(u8.subarray(o, o + n));
    let s = ''; for (let i = 0; i < n; i++) s += String.fromCharCode(u8[o + i]); return s;
  }

  function extractMap(ab) {
    const plain = decryptBFD(ab);
    const dv = new DataView(plain.buffer, plain.byteOffset, plain.byteLength);
    const version = dv.getUint32(0, true);
    const count = dv.getUint32(8, true);
    const objects = [];
    let base = 0xC;
    for (let i = 0; i < count; i++) {
      const o = base + i * REC;
      if (o + REC > plain.length) break;
      const nm = name(plain, o);
      let x = dv.getFloat32(o + POS, true), y = dv.getFloat32(o + POS + 4, true), z = dv.getFloat32(o + POS + 8, true);
      if (!(Number.isFinite(x) && Number.isFinite(y) && Number.isFinite(z)) || Math.abs(x) > 1e7) { x = y = z = 0; }
      objects.push({ name: nm, pos: [x, y, z] });
    }
    return { version: '0x' + version.toString(16), count, objects };
  }

  root.AXISMap = { decryptBFD, extractMap };
  if (typeof module !== 'undefined' && module.exports) module.exports = { decryptBFD, extractMap };
})(typeof window !== 'undefined' ? window : globalThis);
