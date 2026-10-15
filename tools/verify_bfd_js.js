'use strict';
// Verify web/bfd.js decrypts + extracts map objects like tools/extract_map.py.
// Prints version/count and the first object for each map; compare to extract_map.py.
//   node verify_bfd_js.js <game_dir> [map.bfd ...]
const fs = require('fs'), path = require('path');
const { extractMap } = require('../web/bfd.js');
const game = process.argv[2];
const mapdir = path.join(game, 'Map');
const maps = process.argv.slice(3).length ? process.argv.slice(3)
  : fs.readdirSync(mapdir).filter(f => f.toLowerCase().endsWith('.bfd'));
for (const f of maps) {
  const buf = fs.readFileSync(path.join(mapdir, f));
  const ab = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
  const m = extractMap(ab);
  const o = m.objects[0];
  console.log(`${f}: ver=${m.version} count=${m.count} first=${o ? o.name + '@(' + (o.pos[0] | 0) + ',' + (o.pos[1] | 0) + ',' + (o.pos[2] | 0) + ')' : '-'}`);
}
