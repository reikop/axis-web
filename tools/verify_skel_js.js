'use strict';
// Verify web/skel.js parses skeletons + computes bind-pose joints like
// tools/parse_skeleton.py. Prints bone count, root count, and first/last
// joint world positions; compare against parse_skeleton.py output.
//   node verify_skel_js.js <Skeleton.dat> [name-substr ...]
const fs = require('fs');
const { listSkeletons, parseSkeleton, worldJoints } = require('../web/skel.js');
const buf = fs.readFileSync(process.argv[2]);
const ab = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
const all = listSkeletons(ab);
const wants = process.argv.slice(3);
const pick = wants.length
  ? wants.map(w => all.find(e => e.name.toUpperCase().includes(w.toUpperCase()))).filter(Boolean)
  : all.slice(0, 3);
for (const e of pick) {
  const sk = parseSkeleton(ab, e);
  const w = worldJoints(sk);
  const roots = sk.bones.filter(b => b.parent < 0).length;
  const f = p => `(${p[0].toFixed(1)},${p[1].toFixed(1)},${p[2].toFixed(1)})`;
  console.log(`${sk.name}: bones=${sk.boneCount} roots=${roots}`);
  sk.bones.forEach((b, i) =>
    console.log(`  ${i} ${b.name.padEnd(9)} parent=${String(b.parent).padStart(2)} world=${f(w[i])}`));
}
console.log(`total skeletons: ${all.length}`);
