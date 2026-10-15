/* AXIS Shape.dat archive + .shp mesh reader, in the browser engine.
 *
 * JS port of tools/parse_archive.py + tools/parse_shp.py + tools/export_shapes.py (merge).
 * Lets the client load 3D meshes straight from the user's own Shape.dat in the browser —
 * the asset loading the WASM client (approach B) ultimately does. See docs/SPEC.md §2.1/§2.8.
 *
 *   readTOC(datArrayBuffer)                 -> [{name, off, end}]
 *   parseShape(datArrayBuffer, off, end)    -> {pos:[[x,y,z]], nrm, uv, tris:[[a,b,c]]}  (groups merged)
 */
(function (root) {
  'use strict';
  const dec = (typeof TextDecoder !== 'undefined') ? new TextDecoder('euc-kr') : null;
  const REC = 0x44, NAME = 0x40, OBJ_HDR = 0x4c, GRP_HDR = 0x80, TAG = 0x71110300;

  function name64(u8, o) {
    let n = 0; while (n < NAME && u8[o + n]) n++;
    if (dec) return dec.decode(u8.subarray(o, o + n));
    let s = ''; for (let i = 0; i < n; i++) s += String.fromCharCode(u8[o + i]); return s;
  }

  function readTOC(ab) {
    const dv = new DataView(ab), u8 = new Uint8Array(ab), count = dv.getUint32(0, true), out = [];
    for (let i = 0; i < count; i++) {
      const b = 4 + i * REC;
      out.push({ name: name64(u8, b), off: dv.getUint32(b + NAME, true) });
    }
    for (let i = 0; i < out.length; i++) out[i].end = (i + 1 < out.length) ? out[i + 1].off : ab.byteLength;
    return out;
  }

  function parseShape(ab, off, end) {
    const dv = new DataView(ab);
    const tag = dv.getUint32(off + 0x30, true);
    if (tag !== TAG) throw new Error('tag ' + tag.toString(16));
    const gcount = dv.getInt32(off + 0x10, true);
    const meta = [];
    for (let g = 0; g < gcount; g++) {
      const gh = off + OBJ_HDR + g * GRP_HDR;
      meta.push(dv.getInt32(gh + 0x34, true));   // vertexCount
    }
    let p = off + OBJ_HDR + gcount * GRP_HDR;
    const pos = [], nrm = [], uv = [], tris = [];
    for (let g = 0; g < gcount; g++) {
      const vc = meta[g], base = pos.length;
      for (let i = 0; i < vc; i++) pos.push([dv.getFloat32(p + i * 12, true), dv.getFloat32(p + i * 12 + 4, true), dv.getFloat32(p + i * 12 + 8, true)]);
      p += vc * 12;
      for (let i = 0; i < vc; i++) nrm.push([dv.getFloat32(p + i * 12, true), dv.getFloat32(p + i * 12 + 4, true), dv.getFloat32(p + i * 12 + 8, true)]);
      p += vc * 12;
      p += vc * 4;                                // attr
      for (let i = 0; i < vc; i++) uv.push([dv.getFloat32(p + i * 8, true), dv.getFloat32(p + i * 8 + 4, true)]);
      p += vc * 8;
      // index region: triangles (3xu32 all < vc) interleaved with 12-byte descriptors
      // whose 3rd word is a stale saved pointer (0x04000000..0x40000000).
      while (p + 12 <= end) {
        const a = dv.getUint32(p, true), b = dv.getUint32(p + 4, true), c = dv.getUint32(p + 8, true);
        if (a < vc && b < vc && c < vc) { tris.push([a + base, b + base, c + base]); p += 12; }
        else if (c > 0x04000000 && c < 0x40000000) { p += 12; }
        else break;
      }
    }
    return { pos, nrm, uv, tris, consumed: p - off, size: end - off };
  }

  root.AXISShape = { readTOC, parseShape };
  if (typeof module !== 'undefined' && module.exports) module.exports = { readTOC, parseShape };
})(typeof window !== 'undefined' ? window : globalThis);
