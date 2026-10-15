#!/usr/bin/env python3
"""Decode an AXIS PTX texture (from an Image*.dat entry) to RGBA / PNG.

PTX is a baseline JPEG under a custom 16-byte header (see docs/SPEC.md §2.2):
full-resolution Y, Cb, Cr (+ optional alpha) planes, each 8x8-block DCT with
differential-DC + run-length-AC Huffman in JPEG zig-zag order, quantised by the
standard JPEG tables scaled by the header's quality field.

The two Huffman tables are specific to AXIS and are read at run time from YOUR
own AXIS.EXE (nothing game-specific is embedded here). The quant tables and the
zig-zag scan are the public JPEG standard ones.

    python -I ptx_decode.py <Image.dat> <offset> <size> <AXIS.EXE> <out.png>
"""
import sys
import struct
import zlib

# public JPEG standard zig-zag and quant tables (Annex K)
ZIGZAG = [0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,
          13,6,7,14,21,28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,
          52,45,38,31,39,46,53,60,61,54,47,55,62,63]
QLUMA = [16,11,10,16,24,40,51,61,12,12,14,19,26,58,60,55,14,13,16,24,40,57,69,56,
         14,17,22,29,51,87,80,62,18,22,37,56,68,109,103,77,24,35,55,64,81,104,113,92,
         49,64,78,87,103,121,120,101,72,92,95,98,112,100,103,99]
QCHROMA = [17,18,24,47,99,99,99,99,18,21,26,66,99,99,99,99,24,26,56,99,99,99,99,99,
           47,66,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,
           99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99]

# Huffman table virtual addresses inside AXIS.EXE (image base 0x400000)
DC_VA, DC_STRIDE = 0x4c6a98, 0x60      # 2 variants: luma, chroma
AC_VA, AC_STRIDE = 0x4c6b58, 0x800


def va_to_off(exe, va):
    """Convert a virtual address to a file offset using the PE section table."""
    e_lfanew = struct.unpack_from("<I", exe, 0x3c)[0]
    nsec = struct.unpack_from("<H", exe, e_lfanew + 6)[0]
    opt = struct.unpack_from("<H", exe, e_lfanew + 20)[0]        # SizeOfOptionalHeader
    base = struct.unpack_from("<I", exe, e_lfanew + 24 + 28)[0]  # ImageBase (PE32)
    rva = va - base
    sec = e_lfanew + 24 + opt
    for i in range(nsec):
        s = sec + i * 40
        vsize = struct.unpack_from("<I", exe, s + 8)[0]
        vaddr = struct.unpack_from("<I", exe, s + 12)[0]
        praw = struct.unpack_from("<I", exe, s + 20)[0]
        if vaddr <= rva < vaddr + max(vsize, 1) + 0x2000:
            return praw + (rva - vaddr)
    raise ValueError("VA not in any section")


def read_huff(exe, va, n_entries):
    off = va_to_off(exe, va)
    out = []
    for i in range(n_entries):
        ln, code = struct.unpack_from("<ii", exe, off + i * 8)
        out.append((ln, code))
    return out


class Bits:
    def __init__(self, data, start):
        self.d = data
        self.p = start * 8   # bit position

    def read(self, n):
        v = 0
        for _ in range(n):
            byte = self.d[self.p >> 3]
            bit = (byte >> (7 - (self.p & 7))) & 1
            v = (v << 1) | bit
            self.p += 1
        return v

    def peek16(self):
        save = self.p
        v = self.read(16)
        self.p = save
        return v


def extend(v, n):
    return v if v >= (1 << (n - 1)) else v - (1 << n) + 1


def huff_dc(bits, table):
    peek = bits.peek16()
    for cat, (ln, code) in enumerate(table):
        if ln and (peek >> (16 - ln)) == code:
            bits.p += ln
            if cat == 0:
                return 0
            return extend(bits.read(cat), cat)
    raise ValueError("bad DC code")


def huff_ac(bits, table):
    peek = bits.peek16()
    for sym, (ln, code) in enumerate(table):
        if ln and (peek >> (16 - ln)) == code:
            bits.p += ln
            run, size = sym >> 4, sym & 0xf
            if size == 0:
                return run, 0   # EOB (sym 0) or ZRL (sym 0xF0)
            return run, extend(bits.read(size), size)
    raise ValueError("bad AC code")


import math as _math
_C = [_math.sqrt(1 / 8.0)] + [_math.sqrt(2 / 8.0)] * 7
# separable IDCT basis: M[x][u] = C[u]*cos((2x+1)u*pi/16)
_M = [[_C[u] * _math.cos((2 * x + 1) * u * _math.pi / 16) for u in range(8)] for x in range(8)]


def idct8(block):
    # rows then columns (separable), skipping all-zero rows
    tmp = [0.0] * 64
    for v in range(8):
        row = block[v * 8:v * 8 + 8]
        if not any(row):
            continue
        for x in range(8):
            Mx = _M[x]
            tmp[v * 8 + x] = (row[0] * Mx[0] + row[1] * Mx[1] + row[2] * Mx[2] + row[3] * Mx[3]
                              + row[4] * Mx[4] + row[5] * Mx[5] + row[6] * Mx[6] + row[7] * Mx[7])
    out = [0.0] * 64
    for x in range(8):
        col = [tmp[v * 8 + x] for v in range(8)]
        if not any(col):
            continue
        for y in range(8):
            My = _M[y]
            out[y * 8 + x] = (col[0] * My[0] + col[1] * My[1] + col[2] * My[2] + col[3] * My[3]
                              + col[4] * My[4] + col[5] * My[5] + col[6] * My[6] + col[7] * My[7])
    return out


def decode(ptx, exe):
    assert ptx[:4] == b"PTX\x00", "not PTX"
    ver, w8, h8, fmt, quality, _ = struct.unpack_from("<HHHHHH", ptx, 4)
    w, h = w8 * 8, h8 * 8
    planes = 4 if fmt == 0x82 else 3
    dc = [read_huff(exe, DC_VA + v * DC_STRIDE, 12) for v in range(2)]
    ac = [read_huff(exe, AC_VA + v * AC_STRIDE, 256) for v in range(2)]
    # per-plane quant (plain, for a plain IDCT): jpegQuant * 100 / quality
    ql = [max(1, (QLUMA[i] * 100) // quality) for i in range(64)]
    qc = [max(1, (QCHROMA[i] * 100) // quality) for i in range(64)]

    bx, by = w // 8, h // 8
    bits = Bits(ptx + b"\x00" * 64, 16)   # pad so peek16 never overruns
    total_bits = len(ptx) * 8
    plane_px = []
    overrun = False
    for p in range(planes):
        mode = 1 if p in (1, 2) else 0
        q = qc if mode else ql
        pred = 0
        px = [128] * (w * h)
        for b in range(bx * by):
            if bits.p > total_bits + 16:
                print(f"  OVERRUN at plane {p} block {b}/{bx*by} bit {bits.p}/{total_bits}")
                overrun = True
                break
            coef = [0] * 64
            diff = huff_dc(bits, dc[mode])
            pred += diff
            coef[0] = pred
            k = 1
            while k < 64:
                run, val = huff_ac(bits, ac[mode])
                if val == 0 and run == 0:
                    break            # EOB
                k += run
                if k >= 64:
                    break
                coef[ZIGZAG[k]] = val
                k += 1
            deq = [coef[i] * q[i] for i in range(64)]
            blk = idct8(deq)
            ox, oy = (b % bx) * 8, (b // bx) * 8
            for yy in range(8):
                for xx in range(8):
                    v = int(round(blk[yy * 8 + xx])) + 128
                    px[(oy + yy) * w + (ox + xx)] = 0 if v < 0 else 255 if v > 255 else v
        plane_px.append(px)
        bits.p = (bits.p + 7) & ~7   # byte-align before next plane
        if overrun:
            while len(plane_px) < planes:
                plane_px.append([128] * (w * h))
            break

    # YCbCr(+A) -> RGBA
    Y, Cb, Cr = plane_px[0], plane_px[1], plane_px[2]
    A = plane_px[3] if planes == 4 else None
    rgba = bytearray(w * h * 4)
    for i in range(w * h):
        yv, cb, cr = Y[i], Cb[i] - 128, Cr[i] - 128
        r = yv + 1.402 * cr
        g = yv - 0.344136 * cb - 0.714136 * cr
        bl = yv + 1.772 * cb
        j = i * 4
        rgba[j] = 0 if r < 0 else 255 if r > 255 else int(r)
        rgba[j + 1] = 0 if g < 0 else 255 if g > 255 else int(g)
        rgba[j + 2] = 0 if bl < 0 else 255 if bl > 255 else int(bl)
        rgba[j + 3] = A[i] if A else 255
    return w, h, bytes(rgba)


def write_png(path, w, h, rgba):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgba[y * w * 4:(y + 1) * w * 4]
    comp = zlib.compress(bytes(raw), 9)

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", comp))
        f.write(chunk(b"IEND", b""))


def main():
    if len(sys.argv) != 6:
        print(__doc__)
        return 1
    path, off, size, exe_path, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5]
    ptx = open(path, "rb").read()[off:off + size]
    exe = open(exe_path, "rb").read()
    w, h, rgba = decode(ptx, exe)
    write_png(out, w, h, rgba)
    print(f"decoded {w}x{h} PTX -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
