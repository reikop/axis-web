/* AXIS Skeleton.dat reader (bind-pose skeletons), in the browser engine.
 *
 * JS port of tools/parse_skeleton.py (see docs/SPEC.md §2.10). Reads the
 * plaintext indexed archive TOC, parses a skeleton's bone records, and builds
 * bind-pose world joint positions. Lets the client load a skeleton straight
 * from the user's own Data/Skeleton.dat in the browser.
 *
 *   listSkeletons(ab) -> [{name, offset, size}]
 *   parseSkeleton(ab, entry) -> {name, boneCount, bones:[{name,parent,t,angle,axis}]}
 *   worldJoints(sk) -> [[x,y,z], ...]   bind-pose joint positions, bone order
 */
(function (root) {
  'use strict';
  const dec = (typeof TextDecoder !== 'undefined') ? new TextDecoder('euc-kr') : null;
  const REC = 0x44, NAME = 0x40, STRIDE = 0x70, FIRST = 0x24;

  function cstr(u8, o, max) {
    let n = 0; while (n < max && u8[o + n]) n++;
    if (dec) return dec.decode(u8.subarray(o, o + n));
    let s = ''; for (let i = 0; i < n; i++) s += String.fromCharCode(u8[o + i]); return s;
  }

  function listSkeletons(ab) {
    const u8 = new Uint8Array(ab), dv = new DataView(ab);
    const count = dv.getUint32(0, true);
    const ent = [];
    for (let i = 0; i < count; i++) {
      const base = 4 + i * REC;
      ent.push({ name: cstr(u8, base, NAME), offset: dv.getUint32(base + NAME, true) });
    }
    const out = [];
    for (let i = 0; i < ent.length; i++) {
      const end = i + 1 < ent.length ? ent[i + 1].offset : u8.length;
      out.push({ name: ent[i].name, offset: ent[i].offset, size: end - ent[i].offset });
    }
    return out;
  }

  function parseSkeleton(ab, entry) {
    const u8 = new Uint8Array(ab, entry.offset, entry.size);
    const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
    const boneCount = dv.getInt32(0x10, true);
    const bones = [];
    for (let b = 0; b < boneCount; b++) {
      const bo = FIRST + b * STRIDE;
      if (bo + STRIDE > u8.length) break;
      bones.push({
        name: cstr(u8, bo, NAME),
        parent: dv.getInt32(bo + 0x40, true),
        t: [dv.getFloat32(bo + 0x44, true), dv.getFloat32(bo + 0x48, true), dv.getFloat32(bo + 0x4c, true)],
        angle: dv.getFloat32(bo + 0x50, true),
        axis: [dv.getFloat32(bo + 0x54, true), dv.getFloat32(bo + 0x58, true), dv.getFloat32(bo + 0x5c, true)]
      });
    }
    return { name: entry.name, boneCount, bones };
  }

  function axisAngle(th, ax, ay, az) {
    let n = Math.hypot(ax, ay, az) || 1; ax /= n; ay /= n; az /= n;
    const c = Math.cos(th), s = Math.sin(th), t = 1 - c;
    return [t*ax*ax+c, t*ax*ay-s*az, t*ax*az+s*ay, 0,
            t*ax*ay+s*az, t*ay*ay+c, t*ay*az-s*ax, 0,
            t*ax*az-s*ay, t*ay*az+s*ax, t*az*az+c, 0,
            0, 0, 0, 1];
  }
  function mul(a, b) {
    const r = new Array(16);
    for (let i = 0; i < 4; i++) for (let j = 0; j < 4; j++)
      r[i*4+j] = a[i*4]*b[j] + a[i*4+1]*b[4+j] + a[i*4+2]*b[8+j] + a[i*4+3]*b[12+j];
    return r;
  }

  function worldJoints(sk) {
    const n = sk.bones.length, world = new Array(n).fill(null);
    function comp(i) {
      if (world[i]) return world[i];
      const b = sk.bones[i];
      const tl = [1,0,0,b.t[0], 0,1,0,b.t[1], 0,0,1,b.t[2], 0,0,0,1];
      const local = mul(tl, axisAngle(b.angle, b.axis[0], b.axis[1], b.axis[2]));
      world[i] = (b.parent < 0 || b.parent >= n) ? local : mul(comp(b.parent), local);
      return world[i];
    }
    const out = [];
    for (let i = 0; i < n; i++) { const m = comp(i); out.push([m[3], m[7], m[11]]); }
    return out;
  }

  root.AXISSkel = { listSkeletons, parseSkeleton, worldJoints };
  if (typeof module !== 'undefined' && module.exports)
    module.exports = { listSkeletons, parseSkeleton, worldJoints };
})(typeof window !== 'undefined' ? window : globalThis);
