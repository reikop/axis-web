'use strict';
// Verify web/ape.js extracts the same trajectories + names as tools/parse_ape.py.
//   node verify_ape_js.js <file.ape>
const fs = require('fs');
const { parseReplay } = require('../web/ape.js');
const buf = fs.readFileSync(process.argv[2]);
const ab = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
const r = parseReplay(ab);
const types = {}; Object.keys(r.types).sort((a, b) => a - b).forEach(k => types[k] = r.types[k]);
console.log(`records=${r.records} start=${r.start}`);
console.log('types:', JSON.stringify(types));
console.log('names:', JSON.stringify(r.names));
for (const a of Object.keys(r.tracks).sort()) {
  const pts = r.tracks[a];
  const xs = pts.map(p => p[1]), ys = pts.map(p => p[2]), zs = pts.map(p => p[3]);
  const mm = v => `[${Math.min(...v)},${Math.max(...v)}]`;
  console.log(`  actor ${a}: ${pts.length} positions  x${mm(xs)} y${mm(ys)} z${mm(zs)}`);
}
console.log('first 8 of busiest actor:',
  JSON.stringify((r.tracks[Object.keys(r.tracks).sort((a, b) => r.tracks[b].length - r.tracks[a].length)[0]] || []).slice(0, 8).map(p => p.slice(1))));
