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
        rngFill = F('axis_rng_fill'), rngDraw = F('axis_rng_draw'), shpParse = F('axis_shp_parse'), bfdExtract = F('axis_bfd_extract'),
        ptxDecode = F('axis_ptx_decode'), skelParse = F('axis_skel_parse'), apeParse = F('axis_ape_parse');
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

    // ---- axis_rng_draw: the lockstep draw primitive, reproduced on a REAL match.
    // A type-1 packet's RNG fields are two successive pool draws (SPEC §4,
    // FUN_0048d080): [44]=pool[idx]%100000, [50]=pool[idx+1]%100000. Recover each
    // packet's idx from the pool and check the WASM draw reproduces [44]/[50].
    if (typeof rngDraw === 'function') {
      let AP2; try { AP2 = require(path.join(__dirname, '..', '..', 'web', 'ape.js')); } catch (e) { AP2 = null; }
      const ape = path.join(game, 'AutoPlay', 'Test.ape');
      if (AP2 && fs.existsSync(ape)) {
        const pairIdx = new Map();
        for (let i = 0; i < 4096; i++) { const k = pool[i] * 100000 + pool[(i + 1) & 0xfff]; if (!pairIdx.has(k)) pairIdx.set(k, i); }
        const raw = fs.readFileSync(ape);
        const u8 = AP2.decrypt(raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength));
        const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
        let start = -1;
        for (let o = 0; o < Math.min(u8.length, 4096) && start < 0; o++) { let ok = true; for (let k = 0; k < 3; k++) { const b = o + k * 76; if (b + 76 > u8.length || dv.getUint32(b + 8, true) !== 0x40) { ok = false; break; } } if (ok) start = o; }
        const IDX = RB + 4096 * 4;                 // scratch u32 for the draw index
        const idxV = new Uint32Array(memory.buffer, IDX, 1);
        let drew = 0, okd = 0;
        for (let o = start; o >= 0 && o + 76 <= u8.length; o += 76) {
          if (dv.getUint32(o + 8, true) !== 0x40) break;
          const p = o + 12; if (u8[p + 1] !== 1) continue;
          const v44 = dv.getUint16(p + 44, true), v50 = dv.getUint16(p + 50, true);
          if ((v44 === 0 && v50 === 0) || v44 >= 100000 || v50 >= 100000) continue;
          const i0 = pairIdx.get(v44 * 100000 + v50); if (i0 === undefined) continue;
          drew++;
          idxV[0] = i0;
          const d1 = rngDraw(RB, IDX, 100000), d2 = rngDraw(RB, IDX, 100000);
          if (d1 === v44 && d2 === v50 && idxV[0] === ((i0 + 2) & 0xffffffff)) okd++;
        }
        console.log(`rng-draw (WASM) on Test.ape: ${okd}/${drew} type-1 packets' [44]/[50] reproduced by the pool draw`);
        if (drew === 0 || okd !== drew) fails++;
      }
    }
  }

  // ---- axis_shp_parse: WASM mesh decoder vs web/shp.js over every Shape.dat record
  const shapeDat = path.join(game, 'Data', 'Shape.dat');
  if (typeof shpParse === 'function' && fs.existsSync(shapeDat)) {
    let SHP;
    try { SHP = require(path.join(__dirname, '..', '..', 'web', 'shp.js')); }
    catch (e) { console.log('shp.js not loadable — skipping mesh check:', e.message); SHP = null; }
    if (SHP) {
      const raw = fs.readFileSync(shapeDat);
      const ab = raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength);
      const toc = SHP.readTOC(ab);
      // memory layout: file at FILE, output buffers above it (no grow inside loop)
      const FILE = BASE;                                   // 16 MiB
      const VCAP = 200000, TCAP = 400000, GCAP = 4096;
      const POS = 0x1C00000, NRM = POS + VCAP * 12, UV = NRM + VCAP * 12,
            TRI = UV + VCAP * 8, GRP = TRI + TCAP * 12, CNT = GRP + GCAP * 8;
      ensure(CNT + 64);
      new Uint8Array(memory.buffer).set(new Uint8Array(ab), FILE);
      const buf = memory.buffer;
      const posV = new Float32Array(buf, POS, VCAP * 3), nrmV = new Float32Array(buf, NRM, VCAP * 3),
            uvV = new Float32Array(buf, UV, VCAP * 2), triV = new Uint32Array(buf, TRI, TCAP * 3),
            grpV = new Uint32Array(buf, GRP, GCAP * 2), cntV = new Uint32Array(buf, CNT, 4);
      const eqF = (a, b, n) => { for (let i = 0; i < n; i++) if (!Object.is(a[i], b[i])) return i; return -1; };

      let ok = 0, bad = 0, rej = 0, over = 0, firstBad = null;
      for (const t of toc) {
        let js = null, threw = false;
        try { js = SHP.parseShape(ab, t.off, t.end); } catch (e) { threw = true; }
        const ret = shpParse(FILE, t.off, t.end, POS, NRM, UV, TRI, GRP, CNT);
        if (threw) { if (ret === -1) rej++; else { bad++; firstBad = firstBad || `${t.name}: js threw but wasm ret ${ret}`; } continue; }
        if (js.pos.length > VCAP || js.tris.length > TCAP || js.groups.length > GCAP) { over++; continue; }
        const vt = cntV[0], tt = cntV[1], gc = cntV[2], cons = cntV[3];
        let why = null;
        if (ret !== 0) why = `ret ${ret}`;
        else if (vt !== js.pos.length) why = `vcount ${vt}!=${js.pos.length}`;
        else if (tt !== js.tris.length) why = `tcount ${tt}!=${js.tris.length}`;
        else if (gc !== js.groups.length) why = `gcount ${gc}!=${js.groups.length}`;
        else if (cons !== js.consumed) why = `consumed ${cons}!=${js.consumed}`;
        if (!why) {
          const jp = new Float32Array(vt * 3), jn = new Float32Array(vt * 3), ju = new Float32Array(vt * 2), jt = new Uint32Array(tt * 3);
          for (let i = 0; i < vt; i++) { jp[i*3]=js.pos[i][0]; jp[i*3+1]=js.pos[i][1]; jp[i*3+2]=js.pos[i][2];
            jn[i*3]=js.nrm[i][0]; jn[i*3+1]=js.nrm[i][1]; jn[i*3+2]=js.nrm[i][2]; ju[i*2]=js.uv[i][0]; ju[i*2+1]=js.uv[i][1]; }
          for (let i = 0; i < tt; i++) { jt[i*3]=js.tris[i][0]; jt[i*3+1]=js.tris[i][1]; jt[i*3+2]=js.tris[i][2]; }
          let bi;
          if ((bi = eqF(posV, jp, vt*3)) >= 0) why = `pos[${bi}]`;
          else if ((bi = eqF(nrmV, jn, vt*3)) >= 0) why = `nrm[${bi}]`;
          else if ((bi = eqF(uvV, ju, vt*2)) >= 0) why = `uv[${bi}]`;
          else if ((bi = eqF(triV, jt, tt*3)) >= 0) why = `tri[${bi}]`;
          else for (let g = 0; g < gc && !why; g++) if (grpV[g*2] !== js.groups[g].triStart || grpV[g*2+1] !== js.groups[g].triEnd) why = `grp[${g}]`;
        }
        if (why) { bad++; firstBad = firstBad || `${t.name}: ${why}`; } else ok++;
      }
      console.log(`\nshp mesh decode: ${ok}/${toc.length} meshes byte-identical to web/shp.js` +
        ` (${rej} rejected by both as non-mesh${over ? `, ${over} over cap` : ''}${bad ? `, ${bad} MISMATCH` : ''})`);
      if (firstBad) console.log(`  first mismatch: ${firstBad}`);
      fails += bad;
    }
  }

  // ---- axis_bfd_extract: WASM map decoder vs web/bfd.js over every Map/*.bfd
  const mapDir2 = path.join(game, 'Map');
  if (typeof bfdExtract === 'function' && fs.existsSync(mapDir2)) {
    let MAP;
    try { MAP = require(path.join(__dirname, '..', '..', 'web', 'bfd.js')); }
    catch (e) { console.log('bfd.js not loadable — skipping map check:', e.message); MAP = null; }
    if (MAP) {
      const BF = BASE, BP = 0x1400000, BC = 0x1480000, OCAP = 50000;   // file, pos-out, counts
      ensure(BC + 64);
      let ok = 0, bad = 0, nmaps = 0, firstBad = null;
      for (const fn of fs.readdirSync(mapDir2).filter(f => /\.bfd$/i.test(f))) {
        const raw = fs.readFileSync(path.join(mapDir2, fn));
        const ab = raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength);
        const js = MAP.extractMap(ab);                       // JS: decrypt + parse
        if (js.objects.length > OCAP) { continue; }
        new Uint8Array(memory.buffer).set(new Uint8Array(ab), BF);   // WASM decrypts in place
        bfdExtract(BF, raw.length, BP, BC);
        const cnt = new Uint32Array(memory.buffer, BC, 3);
        const posV = new Float32Array(memory.buffer, BP, js.objects.length * 3);
        nmaps++;
        let why = null;
        const jsVer = parseInt(js.version, 16) >>> 0;
        if (cnt[0] !== jsVer) why = `version ${cnt[0].toString(16)}!=${js.version}`;
        else if (cnt[1] !== js.count) why = `count ${cnt[1]}!=${js.count}`;
        else if (cnt[2] !== js.objects.length) why = `emitted ${cnt[2]}!=${js.objects.length}`;
        else for (let i = 0; i < js.objects.length && !why; i++) {
          const p = js.objects[i].pos;
          if (!Object.is(posV[i*3], p[0]) || !Object.is(posV[i*3+1], p[1]) || !Object.is(posV[i*3+2], p[2]))
            why = `${fn} obj[${i}] pos`;
        }
        if (why) { bad++; firstBad = firstBad || `${fn}: ${why}`; } else ok++;
      }
      console.log(`\nbfd map decode : ${ok}/${nmaps} maps byte-identical to web/bfd.js (version+count+all object positions)${bad ? `, ${bad} MISMATCH` : ''}`);
      if (firstBad) console.log(`  first mismatch: ${firstBad}`);
      fails += bad;
    }
  }

  // ---- axis_ptx_decode: WASM texture decoder vs web/ptx.js over real Image*.dat PTX entries
  const exeFile = path.join(game, 'AXIS.EXE');
  if (typeof ptxDecode === 'function' && fs.existsSync(exeFile)) {
    let PTX, SHP2;
    try { PTX = require(path.join(__dirname, '..', '..', 'web', 'ptx.js')); SHP2 = require(path.join(__dirname, '..', '..', 'web', 'shp.js')); }
    catch (e) { console.log('ptx.js/shp.js not loadable — skipping texture check:', e.message); PTX = null; }
    if (PTX) {
      const exeRaw = fs.readFileSync(exeFile);
      const exeAb = exeRaw.buffer.slice(exeRaw.byteOffset, exeRaw.byteOffset + exeRaw.byteLength);
      const EXEB = 0x1000000, PTXB = 0x1800000, RGBAB = 0x1900000, WHB = 0x1E00000;
      ensure(WHB + 64);
      new Uint8Array(memory.buffer).set(new Uint8Array(exeAb), EXEB);
      const exeU8 = new Uint8Array(exeAb);
      let ok = 0, bad = 0, nonptx = 0, tested = 0, firstBad = null;
      const PER = 15;                                   // sample per Image archive (keeps JS runtime sane)
      for (let n = 0; n < 6; n++) {
        const dat = path.join(game, 'Data', `Image${n}.dat`);
        if (!fs.existsSync(dat)) continue;
        const raw = fs.readFileSync(dat);
        const ab = raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength);
        const toc = SHP2.readTOC(ab);
        for (let i = 0; i < toc.length && i < PER; i++) {
          const t = toc[i];
          const slice = ab.slice(t.off, t.end);
          const sU8 = new Uint8Array(slice);
          if (!(sU8[0] === 0x50 && sU8[1] === 0x54 && sU8[2] === 0x58 && sU8[3] === 0)) { nonptx++; continue; }
          let js;
          try { js = PTX.decodePTX(slice, exeU8); } catch (e) { nonptx++; continue; }
          if (js.w * js.h > 1024 * 1024) continue;
          new Uint8Array(memory.buffer).set(sU8, PTXB);
          const ret = ptxDecode(PTXB, sU8.length, EXEB, exeU8.length, RGBAB, WHB);
          tested++;
          const wh = new Uint32Array(memory.buffer, WHB, 2);
          let why = null;
          if (ret !== 0) why = `ret ${ret}`;
          else if (wh[0] !== js.w || wh[1] !== js.h) why = `dims ${wh[0]}x${wh[1]}!=${js.w}x${js.h}`;
          else {
            const got = new Uint8Array(memory.buffer, RGBAB, js.w * js.h * 4);
            const jr = js.rgba;
            for (let k = 0; k < jr.length; k++) if (got[k] !== jr[k]) { why = `rgba[${k}] ${got[k]}!=${jr[k]}`; break; }
          }
          if (why) { bad++; firstBad = firstBad || `Image${n}/${t.name}: ${why}`; } else ok++;
        }
      }
      console.log(`\nptx texture decode: ${ok}/${tested} textures byte-identical to web/ptx.js (${nonptx} non-PTX entries skipped)${bad ? `, ${bad} MISMATCH` : ''}`);
      if (firstBad) console.log(`  first mismatch: ${firstBad}`);
      fails += bad;
    }
  }

  // ---- axis_skel_parse: WASM bone decoder vs web/skel.js over every Skeleton.dat rig
  const skelDat = path.join(game, 'Data', 'Skeleton.dat');
  if (typeof skelParse === 'function' && fs.existsSync(skelDat)) {
    let SK;
    try { SK = require(path.join(__dirname, '..', '..', 'web', 'skel.js')); }
    catch (e) { console.log('skel.js not loadable — skipping skeleton check:', e.message); SK = null; }
    if (SK) {
      const raw = fs.readFileSync(skelDat);
      const ab = raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength);
      const list = SK.listSkeletons(ab);
      const SF = BASE, PAR = 0x1400000, FOUT = 0x1440000, SC = 0x14C0000, BCAP = 8192;
      ensure(SC + 64);
      new Uint8Array(memory.buffer).set(new Uint8Array(ab), SF);
      let ok = 0, bad = 0, firstBad = null;
      for (const e of list) {
        const js = SK.parseSkeleton(ab, e);
        if (js.bones.length > BCAP) continue;
        skelParse(SF, e.offset, e.size, PAR, FOUT, SC);
        const cnt = new Uint32Array(memory.buffer, SC, 2);
        const par = new Int32Array(memory.buffer, PAR, js.bones.length);
        const fo = new Float32Array(memory.buffer, FOUT, js.bones.length * 7);
        let why = null;
        if (cnt[1] !== js.bones.length) why = `emitted ${cnt[1]}!=${js.bones.length}`;
        else for (let b = 0; b < js.bones.length && !why; b++) {
          const J = js.bones[b];
          if (par[b] !== J.parent) why = `bone[${b}].parent`;
          else if (!Object.is(fo[b*7], J.t[0]) || !Object.is(fo[b*7+1], J.t[1]) || !Object.is(fo[b*7+2], J.t[2])) why = `bone[${b}].t`;
          else if (!Object.is(fo[b*7+3], J.angle)) why = `bone[${b}].angle`;
          else if (!Object.is(fo[b*7+4], J.axis[0]) || !Object.is(fo[b*7+5], J.axis[1]) || !Object.is(fo[b*7+6], J.axis[2])) why = `bone[${b}].axis`;
        }
        if (why) { bad++; firstBad = firstBad || `${e.name}: ${why}`; } else ok++;
      }
      console.log(`\nskeleton decode: ${ok}/${list.length} rigs byte-identical to web/skel.js (parent+translation+axis-angle)${bad ? `, ${bad} MISMATCH` : ''}`);
      if (firstBad) console.log(`  first mismatch: ${firstBad}`);
      fails += bad;
    }
  }

  // ---- axis_ape_parse: WASM replay decoder vs web/ape.js over every AutoPlay/*.ape
  const apDir2 = path.join(game, 'AutoPlay');
  if (typeof apeParse === 'function' && fs.existsSync(apDir2)) {
    let AP;
    try { AP = require(path.join(__dirname, '..', '..', 'web', 'ape.js')); }
    catch (e) { console.log('ape.js not loadable — skipping replay check:', e.message); AP = null; }
    if (AP) {
      const AF = BASE, ROW = 0x1400000, YAW = 0x1600000, AC = 0x1700000;
      ensure(AC + 64);
      let ok = 0, bad = 0, nfiles = 0, firstBad = null;
      for (const fn of fs.readdirSync(apDir2).filter(f => /\.ape$/i.test(f))) {
        const raw = fs.readFileSync(path.join(apDir2, fn));
        const ab = raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength);
        const js = AP.parseReplay(ab);
        new Uint8Array(memory.buffer).set(new Uint8Array(ab), AF);
        apeParse(AF, raw.length, ROW, YAW, AC);
        const cnt = new Uint32Array(memory.buffer, AC, 3);
        const nrows = cnt[2];
        const rows = new Int32Array(memory.buffer, ROW, nrows * 5);
        const yaws = new Float64Array(memory.buffer, YAW, nrows);
        nfiles++;
        // rebuild per-actor tracks from WASM rows (record order) and compare to ape.js tracks
        const wt = {};
        for (let r = 0; r < nrows; r++) { const a = rows[r*5]; (wt[a] || (wt[a] = [])).push([rows[r*5+1], rows[r*5+2], rows[r*5+3], rows[r*5+4], yaws[r]]); }
        let why = null;
        const jsStart = js.start, wStart = (cnt[0] === 0xffffffff) ? -1 : cnt[0];
        if (wStart !== jsStart) why = `start ${wStart}!=${jsStart}`;
        else if (cnt[1] !== js.records) why = `records ${cnt[1]}!=${js.records}`;
        else {
          const acts = Object.keys(js.tracks);
          for (const a of acts) {
            const jt = js.tracks[a], w = wt[a] || [];
            if (w.length !== jt.length) { why = `actor ${a} len ${w.length}!=${jt.length}`; break; }
            for (let i = 0; i < jt.length; i++) {
              if (w[i][0] !== jt[i][0] || w[i][1] !== jt[i][1] || w[i][2] !== jt[i][2] || w[i][3] !== jt[i][3] || !Object.is(w[i][4], jt[i][4])) { why = `actor ${a} row ${i}`; break; }
            }
            if (why) break;
          }
          if (!why && Object.keys(wt).length !== acts.length) why = `actor count ${Object.keys(wt).length}!=${acts.length}`;
        }
        if (why) { bad++; firstBad = firstBad || `${fn}: ${why}`; } else ok++;
      }
      console.log(`\nape replay decode: ${ok}/${nfiles} replays byte-identical to web/ape.js (start+records+all type-0 trajectories)${bad ? `, ${bad} MISMATCH` : ''}`);
      if (firstBad) console.log(`  first mismatch: ${firstBad}`);
      fails += bad;
    }
  }

  console.log(fails ? `\nFAILED (${fails})` : '\nALL CHECKS PASS (emcc toolchain build verified against real game data)');
  process.exit(fails ? 1 : 0);
}
main().catch(e => { console.error(e); process.exit(1); });
