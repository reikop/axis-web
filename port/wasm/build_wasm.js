'use strict';
/* Build + verify axis_crypt.wasm WITHOUT a C toolchain.
 *
 * Emscripten/clang are not available in every environment, so this hand-
 * assembles the exact same module that ../build.sh would produce from
 * ../src/axis_crypt.c: a WebAssembly module exporting `memory`, `axis_decrypt`
 * and `axis_encrypt(ptr,len,skip)`. It then instantiates it in Node, runs it on
 * the user's real encrypted game files, and asserts the output is byte-identical
 * to the reference cipher (web/bfd.js' rule). This is the approach-B WASM path,
 * proven end-to-end in-environment: real reconstructed AXIS logic running as WASM.
 *
 *   node build_wasm.js [<game_dir>]      # writes axis_crypt.wasm, verifies if game present
 */
const fs = require('fs'), path = require('path');

// ---- tiny WASM encoder -----------------------------------------------------
const uleb = n => { const b = []; do { let x = n & 0x7f; n >>>= 7; if (n) x |= 0x80; b.push(x); } while (n); return b; };
const sleb = n => { const b = []; let more = 1; while (more) { let x = n & 0x7f; n >>= 7; if ((n === 0 && !(x & 0x40)) || (n === -1 && (x & 0x40))) more = 0; else x |= 0x80; b.push(x); } return b; };
const str = s => [s.length, ...[...s].map(c => c.charCodeAt(0))];
const sec = (id, bytes) => [id, ...uleb(bytes.length), ...bytes];
const vec = arr => [...uleb(arr.length), ...arr.flat()];

// value types / opcodes
const I32 = 0x7f, F32 = 0x7d, VOID = 0x40;
const OP = { block:0x02, loop:0x03, end:0x0b, br:0x0c, br_if:0x0d, select:0x1b,
  get:0x20, set:0x21, i32load:0x28, f32load:0x2a, load8u:0x2d, load16s:0x2e,
  i32store:0x36, store8:0x3a, store16:0x3b, f32store:0x38,
  i32:0x41, f32c:0x43, add:0x6a, and:0x71, or:0x72, xor:0x73, shl:0x74, shru:0x76, geu:0x4f,
  fadd:0x92, fsub:0x93, fmul:0x94, fdiv:0x95, flt:0x5d, fgt:0x5e, conv:0xb2, trunc:0xa8 };
const f32c = x => [OP.f32c, ...new Uint8Array(Float32Array.of(x).buffer)];

// params: ptr=0 len=1 skip=2 ; locals: i=3 x=4
const P_PTR = 0, P_LEN = 1, P_SKIP = 2, L_I = 3, L_X = 4;
const addr = () => [OP.get, P_PTR, OP.get, L_I, OP.add];      // ptr + i

function bodyDecrypt() {
  const ins = [
    OP.get, P_SKIP, OP.set, L_I,
    OP.block, VOID,
      OP.loop, VOID,
        OP.get, L_I, OP.get, P_LEN, OP.geu, OP.br_if, 1,
        ...addr(), OP.load8u, 0, 0, OP.i32, ...sleb(0x59), OP.xor, OP.set, L_X,
        ...addr(),
          OP.get, L_X, OP.i32, 1, OP.shl,
          OP.get, L_X, OP.i32, 7, OP.shru,
          OP.or,
        OP.store8, 0, 0,
        OP.get, L_I, OP.i32, 1, OP.add, OP.set, L_I,
        OP.br, 0,
      OP.end,
    OP.end,
    OP.end,
  ];
  return [...vec([[2, I32]]), ...ins];   // locals: 2 x i32
}
function bodyEncrypt() {
  const ins = [
    OP.get, P_SKIP, OP.set, L_I,
    OP.block, VOID,
      OP.loop, VOID,
        OP.get, L_I, OP.get, P_LEN, OP.geu, OP.br_if, 1,
        ...addr(), OP.load8u, 0, 0, OP.set, L_X,
        ...addr(),
          OP.get, L_X, OP.i32, 1, OP.shru,
          OP.get, L_X, OP.i32, 7, OP.shl,
          OP.or,
          OP.i32, ...sleb(0x59), OP.xor,
        OP.store8, 0, 0,
        OP.get, L_I, OP.i32, 1, OP.add, OP.set, L_I,
        OP.br, 0,
      OP.end,
    OP.end,
    OP.end,
  ];
  return [...vec([[2, I32]]), ...ins];
}

// axis_decode_state(p=0, out=1) ; local raw=2 (f32). Mirrors axis_netdecode.c.
function bodyDecodeState() {
  const P = 0, O = 1, RAW = 2;
  const ld16 = off => [OP.get, P, OP.load16s, 1, ...uleb(off), OP.conv, ...f32c(10), OP.fmul];
  const ins = [
    // actor: out[0] = p[4] >> 4
    OP.get, O, OP.get, P, OP.load8u, 0, 4, OP.i32, 4, OP.shru, OP.i32store, 2, 0,
    // x/y/z at out+4/+8/+12 = int16(p+5/7/9) * 10
    OP.get, O, ...ld16(5), OP.f32store, 2, 4,
    OP.get, O, ...ld16(7), OP.f32store, 2, 8,
    OP.get, O, ...ld16(9), OP.f32store, 2, 12,
    // raw = p[0xc] * 14.0625
    OP.get, P, OP.load8u, 0, 0x0c, OP.conv, ...f32c(14.0625), OP.fmul, OP.set, RAW,
    // yaw at out+16 = select(raw-3600, raw, raw>1800) * 0.1
    OP.get, O,
      OP.get, RAW, ...f32c(3600), OP.fsub,
      OP.get, RAW,
      OP.get, RAW, ...f32c(1800), OP.fgt,
      OP.select,
      ...f32c(0.1), OP.fmul,
    OP.f32store, 2, 16,
    OP.end,
  ];
  return [...vec([[1, F32]]), ...ins];
}

// axis_encode_state(in=0, p=1) ; local raw=2 (f32). Inverse of decode; mirrors C.
function bodyEncodeState() {
  const IN = 0, P = 1, RAW = 2;
  // store16 at p+off = trunc(f32load(in+inoff) / 10)
  const q = (inoff, poff) => [OP.get, P, OP.get, IN, OP.f32load, 2, inoff, ...f32c(10), OP.fdiv, OP.trunc, OP.store16, 1, poff];
  const ins = [
    // p[1] = 0  (type)
    OP.get, P, OP.i32, 0, OP.store8, 0, 1,
    // p[4] = (actor & 0xf) << 4
    OP.get, P, OP.get, IN, OP.i32load, 2, 0, OP.i32, 0xf, OP.and, OP.i32, 4, OP.shl, OP.store8, 0, 4,
    // x/y/z -> int16 at p+5/7/9
    ...q(4, 5), ...q(8, 7), ...q(12, 9),
    // raw = yaw * 10
    OP.get, IN, OP.f32load, 2, 16, ...f32c(10), OP.fmul, OP.set, RAW,
    // if raw<0 raw += 3600   (select(raw+3600, raw, raw<0))
    OP.get, RAW, ...f32c(3600), OP.fadd,
    OP.get, RAW,
    OP.get, RAW, ...f32c(0), OP.flt,
    OP.select, OP.set, RAW,
    // p[0xc] = trunc(raw / 14.0625)
    OP.get, P, OP.get, RAW, ...f32c(14.0625), OP.fdiv, OP.trunc, OP.store8, 0, 0x0c,
    OP.end,
  ];
  return [...vec([[1, F32]]), ...ins];
}

function buildModule() {
  const magic = [0x00, 0x61, 0x73, 0x6d], version = [0x01, 0x00, 0x00, 0x00];
  // types: 0 = (i32,i32,i32)->() ; 1 = (i32,i32)->()
  const typeSec = sec(1, vec([
    [0x60, ...vec([I32, I32, I32]), ...vec([])],
    [0x60, ...vec([I32, I32]), ...vec([])],
  ]));
  // funcs: axis_decrypt(t0), axis_encrypt(t0), axis_decode_state(t1), axis_encode_state(t1)
  const funcSec = sec(3, vec([0, 0, 1, 1]));
  // memory: one, min 256 pages (16 MiB) — fits any AXIS file in one buffer
  const memSec = sec(5, vec([[0x00, ...uleb(256)]]));
  const expSec = sec(7, vec([
    [...str('memory'), 0x02, 0],
    [...str('axis_decrypt'), 0x00, 0],
    [...str('axis_encrypt'), 0x00, 1],
    [...str('axis_decode_state'), 0x00, 2],
    [...str('axis_encode_state'), 0x00, 3],
  ]));
  const codeSec = sec(10, vec([
    [...uleb(bodyDecrypt().length), ...bodyDecrypt()],
    [...uleb(bodyEncrypt().length), ...bodyEncrypt()],
    [...uleb(bodyDecodeState().length), ...bodyDecodeState()],
    [...uleb(bodyEncodeState().length), ...bodyEncodeState()],
  ]));
  return Uint8Array.from([...magic, ...version, ...typeSec, ...funcSec, ...memSec, ...expSec, ...codeSec]);
}

// reference cipher (same rule as web/bfd.js / tools/bfd_crypt.py)
const refDec = (b, skip) => { const o = Uint8Array.from(b); for (let i = skip; i < o.length; i++) { const x = (o[i] ^ 0x59) & 0xff; o[i] = ((x << 1) | (x >> 7)) & 0xff; } return o; };

async function main() {
  const wasm = buildModule();
  const outPath = path.join(__dirname, 'axis_crypt.wasm');
  fs.writeFileSync(outPath, wasm);
  console.log(`built ${outPath} (${wasm.length} bytes)`);

  // emit a browser loader with the module bytes embedded (so the client needs
  // no separate .wasm fetch and nothing binary is committed — crypt.js is source)
  const b64 = Buffer.from(wasm).toString('base64');
  const cryptJs = `/* AXIS decrypt cipher as WebAssembly, for the browser client.
 * Generated by port/wasm/build_wasm.js from port/src/axis_crypt.c (docs/SPEC.md §2.3).
 * The ${wasm.length}-byte module is embedded below; AXISCrypt.load() returns a
 * WASM-backed { decrypt(u8,skip), encrypt(u8,skip) } (falls back to JS on failure).
 */
(function (root) {
  'use strict';
  const B64 = "${b64}";
  function bytes() { const s = atob(B64), a = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i); return a; }
  function jsDecrypt(u8, skip) { skip = skip || 0; for (let i = skip; i < u8.length; i++) { const x = (u8[i] ^ 0x59) & 0xff; u8[i] = ((x << 1) | (x >> 7)) & 0xff; } return u8; }
  function jsEncrypt(u8, skip) { skip = skip || 0; for (let i = skip; i < u8.length; i++) { let x = u8[i] & 0xff; x = ((x >> 1) | (x << 7)) & 0xff; u8[i] = (x ^ 0x59) & 0xff; } return u8; }
  let mod = null;
  async function load() {
    if (mod) return mod;
    try {
      const { instance } = await WebAssembly.instantiate(bytes(), {});
      const mem = instance.exports.memory, dec = instance.exports.axis_decrypt, enc = instance.exports.axis_encrypt;
      const run = fn => (u8, skip) => {
        const need = (u8.length + 0xffff) >> 16; const have = mem.buffer.byteLength >> 16;
        if (need > have) mem.grow(need - have);
        const m = new Uint8Array(mem.buffer); m.set(u8, 0); fn(0, u8.length, skip || 0);
        u8.set(m.subarray(0, u8.length)); return u8;
      };
      mod = { decrypt: run(dec), encrypt: run(enc), backend: 'wasm' };
    } catch (e) {
      mod = { decrypt: jsDecrypt, encrypt: jsEncrypt, backend: 'js' };
    }
    return mod;
  }
  root.AXISCrypt = { load, jsDecrypt, jsEncrypt };
  if (typeof module !== 'undefined' && module.exports) module.exports = { load, jsDecrypt, jsEncrypt };
})(typeof window !== 'undefined' ? window : globalThis);
`;
  const cryptPath = path.join(__dirname, '..', '..', 'web', 'crypt.js');
  fs.writeFileSync(cryptPath, cryptJs);
  console.log(`emitted ${cryptPath} (browser loader, ${wasm.length}-byte module embedded)`);

  const { instance } = await WebAssembly.instantiate(wasm, {});
  const { memory, axis_decrypt, axis_encrypt } = instance.exports;
  console.log('instantiated; exports:', Object.keys(instance.exports).join(', '));

  // self-test: round-trip on a fixed vector
  const mem = new Uint8Array(memory.buffer);
  const sample = Uint8Array.from([0x00, 0x01, 0x02, 0x59, 0xaa, 0xff, 0x80, 0x7f]);
  mem.set(sample, 0);
  axis_decrypt(0, sample.length, 0);
  const dec = mem.slice(0, sample.length);
  const ref = refDec(sample, 0);
  const decOK = Buffer.compare(Buffer.from(dec), Buffer.from(ref)) === 0;
  axis_encrypt(0, sample.length, 0);                 // invert in place
  const rt = mem.slice(0, sample.length);
  const rtOK = Buffer.compare(Buffer.from(rt), Buffer.from(sample)) === 0;
  console.log(`self-test: decrypt==reference ${decOK ? 'PASS' : 'FAIL'}; encrypt∘decrypt==identity ${rtOK ? 'PASS' : 'FAIL'}`);
  if (!decOK || !rtOK) process.exit(1);

  // verify on the user's real game files, if available
  const game = process.argv[2];
  if (!game) { console.log('no game dir given — skipping real-data verification'); return; }
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
    if (raw.length > 256 * 65536) { console.log(`  skip ${path.basename(file)} (>16MiB)`); continue; }
    const m = new Uint8Array(memory.buffer);
    m.set(raw, 0);
    axis_decrypt(0, raw.length, skip);
    const got = Buffer.from(m.slice(0, raw.length));
    const exp = Buffer.from(refDec(raw, skip));
    const ok = Buffer.compare(got, exp) === 0;
    console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${path.basename(file).padEnd(22)} ${raw.length} bytes (skip ${skip})`);
    ok ? pass++ : fail++;
  }
  console.log(`\ncipher real-data verification: ${pass} pass, ${fail} fail`);
  if (fail) process.exit(1);

  // ---- axis_decode_state: verify the WASM state decoder on real type-0 packets
  const fround = Math.fround;
  const refState = pay => {
    const dv = new DataView(pay.buffer, pay.byteOffset, pay.byteLength);
    const actor = pay[4] >> 4;
    const x = fround(dv.getInt16(5, true) * 10), y = fround(dv.getInt16(7, true) * 10), z = fround(dv.getInt16(9, true) * 10);
    let raw = fround(pay[0x0c] * fround(14.0625)); if (raw > 1800) raw = fround(raw - 3600);
    return { actor, x, y, z, yaw: fround(raw * fround(0.1)) };
  };
  const apeFile = game && path.join(game, 'AutoPlay', 'Autoplay001.ape');
  if (apeFile && fs.existsSync(apeFile)) {
    const raw = fs.readFileSync(apeFile);
    const m = new Uint8Array(memory.buffer);
    m.set(raw, 0);
    axis_decrypt(0, raw.length, 0);                 // WASM decrypt the whole replay
    const dec = m.slice(0, raw.length);
    let start = -1;
    for (let o = 0; o < 4096 && start < 0; o++) {
      let ok = true;
      for (let k = 0; k < 3; k++) { const b = o + k * 76; if (b + 76 > dec.length || new DataView(dec.buffer, b + 8, 4).getUint32(0, true) !== 0x40) { ok = false; break; } }
      if (ok) start = o;
    }
    const fr = Math.fround;
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
    const PB = 1 << 20, OB = PB + 64, EB = PB + 256;
    let sp = 0, sf = 0, ep = 0, ef = 0, rt = 0, shown = 0;
    const FIELDS = [1, 4, 5, 6, 7, 8, 9, 10, 0x0c];
    for (let o = start; o + 76 <= dec.length; o += 76) {
      if (new DataView(dec.buffer, o + 8, 4).getUint32(0, true) !== 0x40) break;
      const pay = dec.slice(o + 12, o + 12 + 64);
      if (pay[1] !== 0) continue;
      let mm = new Uint8Array(memory.buffer); mm.set(pay, PB);
      instance.exports.axis_decode_state(PB, OB);
      const dv = new DataView(memory.buffer);
      const got = { actor: dv.getInt32(OB, true), x: dv.getFloat32(OB + 4, true), y: dv.getFloat32(OB + 8, true), z: dv.getFloat32(OB + 12, true), yaw: dv.getFloat32(OB + 16, true) };
      const exp = refState(pay);
      const okD = got.actor === exp.actor && got.x === exp.x && got.y === exp.y && got.z === exp.z && got.yaw === exp.yaw;
      okD ? sp++ : sf++;
      // encode the decoded struct back to a packet
      instance.exports.axis_encode_state(OB, EB);
      const enc = new Uint8Array(memory.buffer).slice(EB, EB + 64);
      const ref = refEncode(got);
      const okE = FIELDS.every(i => enc[i] === ref[i]);
      okE ? ep++ : ef++;
      // round-trip: does the re-encoded packet recover the original quantised fields?
      if ([5, 6, 7, 8, 9, 10, 0x0c].every(i => enc[i] === pay[i])) rt++;
      if (shown < 3) { console.log(`  state: actor ${got.actor} pos (${got.x},${got.y},${got.z}) yaw ${got.yaw.toFixed(2)}  decode ${okD ? 'PASS' : 'FAIL'}  encode ${okE ? 'PASS' : 'FAIL'}`); shown++; }
    }
    const total = sp + sf;
    console.log(`\nstate-decode  : ${sp}/${total} pass (vs reference, f32-exact)`);
    console.log(`state-encode  : ${ep}/${total} pass (vs reference, f32-exact)`);
    console.log(`encode∘decode : ${rt}/${total} recover the original quantised packet fields`);
    if (sf || ef) process.exit(1);
  }
}
main().catch(e => { console.error(e); process.exit(1); });
