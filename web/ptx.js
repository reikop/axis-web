/* AXIS PTX texture decoder, in the browser engine.
 *
 * A JS port of the reverse-engineered baseline-JPEG codec (see ../docs/SPEC.md §2.2 and
 * ../tools/ptx_decode.py). This is the game's own texture logic running client-side — the
 * kind of reconstructed code the WASM build (approach B) ultimately executes. Verified
 * byte-identical to the Python reference decoder (see ../tools/verify_ptx_js.js).
 *
 * decodePTX(ptxBuf, exeBuf) -> { w, h, rgba: Uint8ClampedArray }
 * Huffman tables are read at run time from the user's own AXIS.EXE (nothing embedded).
 */
(function (root) {
  'use strict';

  const ZIGZAG = [0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,
    13,6,7,14,21,28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,
    39,46,53,60,61,54,47,55,62,63];
  const QLUMA = [16,11,10,16,24,40,51,61,12,12,14,19,26,58,60,55,14,13,16,24,40,57,69,56,
    14,17,22,29,51,87,80,62,18,22,37,56,68,109,103,77,24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101,72,92,95,98,112,100,103,99];
  const QCHROMA = [17,18,24,47,99,99,99,99,18,21,26,66,99,99,99,99,24,26,56,99,99,99,99,99,
    47,66,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99];

  const DC_VA = 0x4c6a98, DC_STRIDE = 0x60;    // 2 Huffman tables: luma, chroma
  const AC_VA = 0x4c6b58, AC_STRIDE = 0x800;

  // precomputed separable IDCT basis M[x][u] = C[u]*cos((2x+1)u*pi/16)
  const M = (function () {
    const C = [Math.sqrt(1 / 8)]; for (let i = 1; i < 8; i++) C.push(Math.sqrt(2 / 8));
    const m = [];
    for (let x = 0; x < 8; x++) { const r = []; for (let u = 0; u < 8; u++) r.push(C[u] * Math.cos((2 * x + 1) * u * Math.PI / 16)); m.push(r); }
    return m;
  })();

  function vaToOff(dv, va) {           // PE VA -> file offset
    const eLfanew = dv.getUint32(0x3c, true);
    const nsec = dv.getUint16(eLfanew + 6, true);
    const opt = dv.getUint16(eLfanew + 20, true);
    const base = dv.getUint32(eLfanew + 24 + 28, true);
    const rva = va - base, sec = eLfanew + 24 + opt;
    for (let i = 0; i < nsec; i++) {
      const s = sec + i * 40;
      const vs = dv.getUint32(s + 8, true), vaddr = dv.getUint32(s + 12, true), praw = dv.getUint32(s + 20, true);
      if (rva >= vaddr && rva < vaddr + Math.max(vs, 1) + 0x2000) return praw + (rva - vaddr);
    }
    throw new Error('VA not in a section');
  }
  function readHuff(dv, va, n) {
    const off = vaToOff(dv, va), t = [];
    for (let i = 0; i < n; i++) t.push([dv.getInt32(off + i * 8, true), dv.getInt32(off + i * 8 + 4, true)]);
    return t;
  }

  function decodePTX(ptxBuf, exeBuf) {
    const b = new Uint8Array(ptxBuf);
    if (!(b[0] === 0x50 && b[1] === 0x54 && b[2] === 0x58 && b[3] === 0)) throw new Error('not PTX');
    const hd = new DataView(ptxBuf);
    const w8 = hd.getUint16(6, true), h8 = hd.getUint16(8, true);
    const fmt = hd.getUint16(10, true), quality = hd.getUint16(12, true);
    const w = w8 * 8, h = h8 * 8, planes = fmt === 0x82 ? 4 : 3;
    const edv = new DataView(exeBuf.buffer || exeBuf, exeBuf.byteOffset || 0);
    const dc = [readHuff(edv, DC_VA, 12), readHuff(edv, DC_VA + DC_STRIDE, 12)];
    const ac = [readHuff(edv, AC_VA, 256), readHuff(edv, AC_VA + AC_STRIDE, 256)];
    const ql = QLUMA.map(q => Math.max(1, (q * 100 / quality) | 0));
    const qc = QCHROMA.map(q => Math.max(1, (q * 100 / quality) | 0));

    // bit reader over the padded payload (MSB-first)
    const data = new Uint8Array(b.length + 64); data.set(b);
    let bp = 16 * 8;                    // start after the 16-byte header
    function read(n) { let v = 0; for (let i = 0; i < n; i++) { v = (v << 1) | ((data[bp >> 3] >> (7 - (bp & 7))) & 1); bp++; } return v >>> 0; }
    function peek16() { const s = bp; const v = read(16); bp = s; return v; }
    function ext(v, n) { return v >= (1 << (n - 1)) ? v : v - (1 << n) + 1; }
    function rhe(x) { const f = Math.floor(x), d = x - f; return d < 0.5 ? f : d > 0.5 ? f + 1 : (f % 2 === 0 ? f : f + 1); }  // round-half-to-even, matching Python round()
    function huffDC(t) { const pk = peek16(); for (let c = 0; c < t.length; c++) { const [ln, code] = t[c]; if (ln && (pk >>> (16 - ln)) === code) { bp += ln; return c === 0 ? 0 : ext(read(c), c); } } throw new Error('DC'); }
    function huffAC(t) { const pk = peek16(); for (let s = 0; s < t.length; s++) { const [ln, code] = t[s]; if (ln && (pk >>> (16 - ln)) === code) { bp += ln; const run = s >> 4, size = s & 15; return [run, size === 0 ? 0 : ext(read(size), size)]; } } throw new Error('AC'); }

    const tmp = new Float64Array(64), out = new Float64Array(64);
    function idct(blk) {
      for (let v = 0; v < 8; v++) { let any = 0; for (let u = 0; u < 8; u++) any |= blk[v * 8 + u]; if (!any) { for (let x = 0; x < 8; x++) tmp[v * 8 + x] = 0; continue; } for (let x = 0; x < 8; x++) { const Mx = M[x]; let s = 0; for (let u = 0; u < 8; u++) s += blk[v * 8 + u] * Mx[u]; tmp[v * 8 + x] = s; } }
      for (let x = 0; x < 8; x++) for (let y = 0; y < 8; y++) { const My = M[y]; let s = 0; for (let v = 0; v < 8; v++) s += tmp[v * 8 + x] * My[v]; out[y * 8 + x] = s; }
    }

    const bx = w >> 3, by = h >> 3, plane = [];
    const totalBits = b.length * 8;
    for (let p = 0; p < planes; p++) {
      const mode = (p === 1 || p === 2) ? 1 : 0, q = mode ? qc : ql;
      let pred = 0; const px = new Uint8ClampedArray(w * h).fill(128); let over = false;
      for (let bb = 0; bb < bx * by && !over; bb++) {
        if (bp > totalBits + 16) { over = true; break; }
        const coef = new Float64Array(64);
        pred += huffDC(dc[mode]); coef[0] = pred;
        let k = 1;
        while (k < 64) { const [run, val] = huffAC(ac[mode]); if (val === 0 && run === 0) break; k += run; if (k >= 64) break; coef[ZIGZAG[k]] = val; k++; }
        for (let i = 0; i < 64; i++) coef[i] *= q[i];
        idct(coef);
        const ox = (bb % bx) * 8, oy = ((bb / bx) | 0) * 8;
        for (let yy = 0; yy < 8; yy++) for (let xx = 0; xx < 8; xx++) px[(oy + yy) * w + (ox + xx)] = rhe(out[yy * 8 + xx]) + 128;
      }
      plane.push(px); bp = (bp + 7) & ~7;
    }

    const Y = plane[0], Cb = plane[1], Cr = plane[2], A = planes === 4 ? plane[3] : null;
    const rgba = new Uint8Array(w * h * 4);          // truncate+clamp to match the reference
    const cl = v => v < 0 ? 0 : v > 255 ? 255 : (v | 0);
    for (let i = 0; i < w * h; i++) {
      const y = Y[i], cb = Cb[i] - 128, cr = Cr[i] - 128, j = i * 4;
      rgba[j] = cl(y + 1.402 * cr); rgba[j + 1] = cl(y - 0.344136 * cb - 0.714136 * cr); rgba[j + 2] = cl(y + 1.772 * cb); rgba[j + 3] = A ? A[i] : 255;
    }
    return { w, h, rgba };
  }

  root.decodePTX = decodePTX;
  if (typeof module !== 'undefined' && module.exports) module.exports = { decodePTX };
})(typeof window !== 'undefined' ? window : globalThis);
