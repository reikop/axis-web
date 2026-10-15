'use strict';
// Verify web/shp.js parses Shape.dat meshes identically to the Python reference.
// Prints v/t counts + a hash of merged vertex positions for a few shapes; compare
// against tools/parse_shp.py (same values => the JS mesh decoder is correct).
//   node verify_shp_js.js <game_dir> [leaf ...]
const fs = require('fs'), path = require('path'), crypto = require('crypto');
const { readTOC, parseShape } = require('../web/shp.js');

const game = process.argv[2];
const want = new Set(process.argv.slice(3).length ? process.argv.slice(3) : ['GIRL', 'ADD01', 'MIDGARD']);
const buf = fs.readFileSync(path.join(game, 'Data', 'Shape.dat'));
const ab = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
for (const e of readTOC(ab)) {
  if (!want.has(e.name.split(/[\\/]/).pop())) continue;
  const r = parseShape(ab, e.off, e.end);
  const fa = new Float32Array(r.pos.length * 3); let k = 0;
  for (const p of r.pos) { fa[k++] = p[0]; fa[k++] = p[1]; fa[k++] = p[2]; }
  const h = crypto.createHash('sha256').update(Buffer.from(fa.buffer)).digest('hex').slice(0, 16);
  console.log(`${e.name}: v=${r.pos.length} t=${r.tris.length} posh=${h} consumed=${r.consumed === r.size}`);
}
