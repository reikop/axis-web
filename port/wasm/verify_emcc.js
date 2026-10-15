'use strict';
/* Verify the Emscripten-built standalone WASM (port/build.sh output) against the
 * user's real game data, exactly as port/wasm/build_wasm.js verifies the hand-
 * assembled module. Run inside WSL (or any box with the emcc build + Node):
 *
 *   node verify_emcc.js <axis.wasm> <game_dir>
 *
 * Checks, all byte/f32-exact vs the independent JS reference:
 *   - axis_decrypt on real Map/*.bfd, AutoPlay/*.ape, Data/Sound.dat
 *   - axis_decode_state / axis_encode_state on real type-0 replay packets
 *   - axis_rng_fill vs the MSVC LCG reference AND the shipped MATHFUNC.TBL pool
 *
 * Unlike the hand-assembled module (which has nothing at linear-memory offset 0),
 * the Emscripten module keeps static data + the C stack near offset 0, so all
 * scratch buffers live at BASE = 16 MiB and up.
 */
const fs = require('fs'), path = require('path');
const fr = Math.fround;
const BASE = 0x1000000;                       // 16 MiB floor, clear of static+stack

const refDec = (b, skip) => { const o = Uint8Array.from(b); for (let i = skip; i < o.length; i++) { const x = (o[i] ^ 0x59) & 0xff; o[i] = ((x << 1) | (x >> 7)) & 0xff; } return o; };
const refState = pay => {
  const dv = new DataView(pay.buffer, pay.byteOffset, pay.byteLength);
  let raw = fr(pay[0x0c] * fr(14.0625)); if (raw > 1800) raw = fr(raw - 3600);
  return { actor: pay[4] >> 4, x: fr(dv.getInt16(5, true) * 10), y: fr(dv.getInt16(7, true) * 10), z: fr(dv.getInt16(9, true) * 10), yaw: fr(raw * fr(0.1)) };
};
const refEncode = s => {
  const o = new Uint8Array(64), dv = new DataView(o.buffer);
  o[1] = 0; o[4] = ((s.actor & 0xf) << 4) & 0xff;
  dv.setInt16(5, Math.trunc(fr(fr(s.x) / 10)) | 0, true);
  dv.setInt16(7, Math.trunc(fr(fr(s.y) / 10)) | 0, true);
  dv.setInt16(9, Math.trunc(fr(fr(s.z) / 10)) | 0, true);
  let raw = fr(fr(s.yaw) * 10); if (raw < 0) raw = fr(raw + 3600);
  o[0x0c] = Math.trunc(fr(raw / fr(14.0625))) & 0xff;
  return o;
};

async function main() {
  const wasmPath = process.argv[2], game = process.argv[3];
  if (!wasmPath) { console.error('usage: node verify_emcc.js <axis.wasm> [game_dir]'); process.exit(2); }
  const wasm = fs.readFileSync(wasmPath);

  // Standalone reactor: satisfy any emscripten/wasi imports with no-ops so it
  // instantiates headless. A pure-leaf module usually needs none, but be safe.
  const imports = { env: {}, wasi_snapshot_preview1: {} };
  const proxy = new Proxy(imports, { get: (t, k) => t[k] || (t[k] = new Proxy({}, { get: () => () => 0 })) });
  let instance;
  try { ({ instance } = await WebAssembly.instantiate(wasm, proxy)); }
  catch (e) { console.error('instantiate failed:', e.message); process.exit(1); }
  const ex = instance.exports;
  const memory = ex.memory;
  console.log('instantiated; exports:', Object.keys(ex).filter(k => /axis|memory/.test(k)).join(', '));
  // resolve exports whether or not emscripten kept the leading underscore
  const F = n => ex[n] || ex['_' + n];
  const decrypt = F('axis_decrypt'), encrypt = F('axis_encrypt'),
        decodeState = F('axis_decode_state'), encodeState = F('axis_encode_state'),
        rngFill = F('axis_rng_fill');
  for (const [n, f] of [['axis_decrypt', decrypt], ['axis_encrypt', encrypt], ['axis_decode_state', decodeState], ['axis_encode_state', encodeState], ['axis_rng_fill', rngFill]])
    if (typeof f !== 'function') { console.error('missing export', n); process.exit(1); }

  const pages = () => memory.buffer.byteLength >> 16;
  const ensure = need => { const have = pages(); const want = (need + 0xffff) >> 16; if (want > have) memory.grow(want - have); };

  let fails = 0;

  // ---- self-test
  {
    const sample = Uint8Array.from([0x00, 0x01, 0x02, 0x59, 0xaa, 0xff, 0x80, 0x7f]);
    ensure(BASE + sample.length);
    new Uint8Array(memory.buffer).set(sample, BASE);
    decrypt(BASE, sample.length, 0);
    const dec = new Uint8Array(memory.buffer).slice(BASE, BASE + sample.length);
    const decOK = Buffer.compare(Buffer.from(dec), Buffer.from(refDec(sample, 0))) === 0;
    encrypt(BASE, sample.length, 0);
    const rt = new Uint8Array(memory.buffer).slice(BASE, BASE + sample.length);
    const rtOK = Buffer.compare(Buffer.from(rt), Buffer.from(sample)) === 0;
    console.log(`self-test: decrypt==reference ${decOK ? 'PASS' : 'FAIL'}; encrypt∘decrypt==identity ${rtOK ? 'PASS' : 'FAIL'}`);
    if (!decOK || !rtOK) fails++;
  }

  if (!game) { console.log('no game dir — skipping real-data checks'); process.exit(fails ? 1 : 0); }

  // ---- cipher on real files
  const targets = [];
  const mapDir = path.join(game, 'Map');
  if (fs.existsSync(mapDir)) for (const f of fs.readdirSync(mapDir).filter(f => /\.bfd$/i.test(f)).slice(0, 5)) targets.push([path.join(mapDir, f), 4]);
  const apDir = path.join(game, 'AutoPlay');
  if (fs.existsSync(apDir)) for (const f of fs.readdirSync(apDir).filter(f => /\.ape$/i.test(f))) targets.push([path.join(apDir, f), 0]);
  const sdat = path.join(game, 'Data', 'Sound.dat');
  if (fs.existsSync(sdat)) targets.push([sdat, 0]);
  let pass = 0, fail = 0;
  for (const [file, skip] of targets) {
    const raw = fs.readFileSync(file);
    ensure(BASE + raw.length);
    new Uint8Array(memory.buffer).set(raw, BASE);
    decrypt(BASE, raw.length, skip);
    const got = Buffer.from(new Uint8Array(memory.buffer).slice(BASE, BASE + raw.length));
    const ok = Buffer.compare(got, Buffer.from(refDec(raw, skip))) === 0;
    console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${path.basename(file).padEnd(22)} ${raw.length} bytes (skip ${skip})`);
    ok ? pass++ : fail++;
  }
  console.log(`cipher real-data: ${pass} pass, ${fail} fail`);
  fails += fail;

  // ---- state decode/encode on real type-0 packets
  const apeFile = path.join(game, 'AutoPlay', 'Autoplay001.ape');
  if (fs.existsSync(apeFile)) {
    const raw = fs.readFileSync(apeFile);
    ensure(BASE + raw.length);
    new Uint8Array(memory.buffer).set(raw, BASE);
    decrypt(BASE, raw.length, 0);
    const dec = new Uint8Array(memory.buffer).slice(BASE, BASE + raw.length);
    let start = -1;
    for (let o = 0; o < 4096 && start < 0; o++) {
      let ok = true;
      for (let k = 0; k < 3; k++) { const b = o + k * 76; if (b + 76 > dec.length || new DataView(dec.buffer, b + 8, 4).getUint32(0, true) !== 0x40) { ok = false; break; } }
      if (ok) start = o;
    }
    const PB = BASE, OB = BASE + 64, EB = BASE + 256;
    let sp = 0, sf = 0, ep = 0, ef = 0, rt = 0, total = 0, shown = 0;
    const FIELDS = [1, 4, 5, 6, 7, 8, 9, 10, 0x0c];
    for (let o = start; o >= 0 && o + 76 <= dec.length; o += 76) {
      if (new DataView(dec.buffer, o + 8, 4).getUint32(0, true) !== 0x40) break;
      const pay = dec.slice(o + 12, o + 12 + 64);
      if (pay[1] !== 0) continue;
      new Uint8Array(memory.buffer).set(pay, PB);
      decodeState(PB, OB);
      const dv = new DataView(memory.buffer);
      const got = { actor: dv.getInt32(OB, true), x: dv.getFloat32(OB + 4, true), y: dv.getFloat32(OB + 8, true), z: dv.getFloat32(OB + 12, true), yaw: dv.getFloat32(OB + 16, true) };
      const exp = refState(pay);
      (got.actor === exp.actor && got.x === exp.x && got.y === exp.y && got.z === exp.z && got.yaw === exp.yaw) ? sp++ : sf++;
      encodeState(OB, EB);
      const enc = new Uint8Array(memory.buffer).slice(EB, EB + 64);
      const ref = refEncode(got);
      FIELDS.every(i => enc[i] === ref[i]) ? ep++ : ef++;
      if ([5, 6, 7, 8, 9, 10, 0x0c].every(i => enc[i] === pay[i])) rt++;
      total++;
      if (shown < 3) { console.log(`  state: actor ${got.actor} pos (${got.x},${got.y},${got.z}) yaw ${got.yaw.toFixed(2)}  decode ${sf ? 'FAIL' : 'PASS'}`); shown++; }
    }
    console.log(`state-decode  : ${sp}/${total} pass (f32-exact)`);
    console.log(`state-encode  : ${ep}/${total} pass (f32-exact)`);
    console.log(`encode∘decode : ${rt}/${total} recover original quantised fields`);
    fails += sf + ef;
  }

  // ---- rng pool
  {
    const RB = BASE;
    ensure(RB + 4096 * 4);
    rngFill(RB);
    const pool = new Uint32Array(memory.buffer.slice(RB, RB + 4096 * 4));
    let seed = 1 >>> 0; const rngRef = new Uint32Array(4096);
    for (let i = 0; i < 4096; i++) { seed = (Math.imul(seed, 214013) + 2531011) >>> 0; rngRef[i] = (seed >>> 16) & 0x7fff; }
    let rp = 0; for (let i = 0; i < 4096; i++) if (pool[i] === rngRef[i]) rp++;
    console.log(`rng-fill (WASM)       : ${rp}/4096 match MSVC rand(srand(1))`);
    if (rp !== 4096) fails++;
    const mt = path.join(game, 'MATHFUNC.TBL');
    if (fs.existsSync(mt)) {
      const r = fs.readFileSync(mt);
      const file = new Uint32Array(r.buffer.slice(r.byteOffset + 3600 * 12, r.byteOffset + 3600 * 12 + 4096 * 4));
      let fp = 0; for (let i = 0; i < 4096; i++) if (pool[i] === file[i]) fp++;
      console.log(`rng-fill vs MATHFUNC.TBL: ${fp}/4096 match shipped game pool`);
      if (fp !== 4096) fails++;
    }
  }

  console.log(fails ? `\nFAILED (${fails})` : '\nALL CHECKS PASS (emcc toolchain build verified against real game data)');
  process.exit(fails ? 1 : 0);
}
main().catch(e => { console.error(e); process.exit(1); });
