'use strict';
// Verify web/ptx.js decodes PTX textures identically to the Python reference.
// Usage: node verify_ptx_js.js <game_dir> <Image.dat> <offset> <size>
const fs = require('fs');
const crypto = require('crypto');
const path = require('path');
const { decodePTX } = require('../web/ptx.js');

const [, , game, imgRel, off, size] = process.argv;
const exe = fs.readFileSync(path.join(game, 'AXIS.EXE'));
const dat = fs.readFileSync(path.join(game, 'Data', imgRel));
const ptx = dat.subarray(parseInt(off, 10), parseInt(off, 10) + parseInt(size, 10));
const ab = ptx.buffer.slice(ptx.byteOffset, ptx.byteOffset + ptx.byteLength);
const r = decodePTX(ab, exe);
const h = crypto.createHash('sha256').update(Buffer.from(r.rgba.buffer)).digest('hex');
console.log(`${r.w}x${r.h} rgba-sha256=${h}`);
