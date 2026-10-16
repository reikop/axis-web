'use strict';
/* List the item/weapon names in the encrypted Data/Item.dat (and Component.dat).
 *
 * These .dat archives use the same per-byte cipher as Map/*.bfd and *.ape
 * (decByte: rol8(b ^ 0x59, 1); the 0x1201 version prefix stays plaintext) — see
 * SPEC §2.3 / web/ape.js. Decrypting reveals readable entry names + resource
 * paths, e.g. the real weapon roster (Item\wapon\*) and pickups (drug\*, Special\*).
 * The web engine's multiplayer weapon list is named from these.
 *
 *   node tools/dump_item_names.js <game_dir> [bytes]
 */
const fs = require('fs'), path = require('path');
function decByte(b) { const x = (b ^ 0x59) & 0xff; return ((x << 1) | (x >> 7)) & 0xff; }
function names(file, n) {
  const fd = fs.openSync(file, 'r'); const raw = Buffer.alloc(n); const got = fs.readSync(fd, raw, 0, n, 0); fs.closeSync(fd);
  const d = Buffer.alloc(got); for (let i = 0; i < got; i++) d[i] = decByte(raw[i]);
  const out = []; let s = '';
  for (let i = 0; i < got; i++) { const c = d[i]; if (c >= 32 && c < 127) s += String.fromCharCode(c); else { if (s.length >= 4) out.push(s); s = ''; } }
  return out;
}
function main() {
  const game = process.argv[2]; const n = parseInt(process.argv[3] || '65536', 10);
  if (!game) { console.error('usage: node dump_item_names.js <game_dir> [bytes]'); process.exit(2); }
  for (const f of ['Item.dat', 'Component.dat']) {
    const p = path.join(game, 'Data', f);
    if (!fs.existsSync(p)) { console.log(f, 'missing'); continue; }
    console.log('=== ' + f + ' (first ' + n + ' bytes) ===');
    const ns = names(p, n);
    // weapon + item resource paths are the interesting ones
    const interesting = ns.filter(s => /wapon|weapon|Item\\|drug|Special|warpzone/i.test(s));
    console.log((interesting.length ? interesting : ns).slice(0, 60).join('\n'));
  }
}
main();
