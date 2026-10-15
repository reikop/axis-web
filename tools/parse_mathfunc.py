#!/usr/bin/env python3
"""Parse MATHFUNC.TBL — AXIS's math lookup tables + the shared RNG pool.

Loaded at startup by AXIS.EXE FUN_0046b6f0 via plain fread (the file is NOT
encrypted). Layout (confirmed: file is exactly 59584 = (3600*3 + 4096)*4 bytes):

    float32 sin[3600]   # DAT_00a1cc48 : sin(i * 0.1 deg)
    float32 cos[3600]   # DAT_00a19408 : cos(i * 0.1 deg)
    float32 tan[3600]   # DAT_00a15bc8 : tan(i * 0.1 deg)
    uint32  rng[4096]   # DAT_00a11bc8 : the shared RNG pool

The RNG pool is the backbone of AXIS's lockstep determinism (SPEC §5): every
peer loads the same pool and consumes it by a single shared index
`pool[idx++ & 0xfff] % N`, so spawns / AI / damage rolls match across clients.
The pool is a precomputed table of standard C `rand()` outputs (0..32767) —
its first entries are the canonical rand() sequence 41,18467,6334,26500,…

This parser VERIFIES the decode: the sin/cos tables must match the real math
functions (to float precision), which proves the layout — and therefore that
the RNG pool is extracted from the correct offset.

    python -I parse_mathfunc.py <MATHFUNC.TBL> [rng_out.json]
"""
import sys
import struct
import math
import json

N = 3600
NP = 4096


def parse(raw):
    sin = struct.unpack_from("<%df" % N, raw, 0)
    cos = struct.unpack_from("<%df" % N, raw, N * 4)
    tan = struct.unpack_from("<%df" % N, raw, N * 8)
    rng = struct.unpack_from("<%dI" % NP, raw, N * 12)
    return sin, cos, tan, rng


def verify(sin, cos, tan):
    es = max(abs(sin[i] - math.sin(math.radians(i * 0.1))) for i in range(N))
    ec = max(abs(cos[i] - math.cos(math.radians(i * 0.1))) for i in range(N))
    # tan diverges near 90 deg; check only where it is well-conditioned
    et = max(abs(tan[i] - math.tan(math.radians(i * 0.1)))
             for i in range(N) if abs(math.cos(math.radians(i * 0.1))) > 0.05)
    return es, ec, et


def main():
    if len(sys.argv) < 2:
        print("usage: python -I parse_mathfunc.py <MATHFUNC.TBL> [rng_out.json]")
        return 1
    raw = open(sys.argv[1], "rb").read()
    print(f"file: {sys.argv[1]}  ({len(raw)} bytes, expected {(N*3+NP)*4})")
    if len(raw) != (N * 3 + NP) * 4:
        print("!! unexpected size — layout may differ")
    sin, cos, tan, rng = parse(raw)
    es, ec, et = verify(sin, cos, tan)
    print(f"sin table: max err vs sin(i*0.1deg) = {es:.3g}   {'OK' if es < 1e-4 else 'MISMATCH'}")
    print(f"cos table: max err vs cos(i*0.1deg) = {ec:.3g}   {'OK' if ec < 1e-4 else 'MISMATCH'}")
    print(f"tan table: max err (|cos|>0.05)     = {et:.3g}   {'OK' if et < 1e-2 else 'MISMATCH'}")
    print(f"\nRNG pool: {NP} entries, range [{min(rng)},{max(rng)}]  "
          f"{'(rand() 0..32767)' if max(rng) <= 32767 else ''}")
    print(f"  first 8 : {list(rng[:8])}")
    print(f"  distinct: {len(set(rng))}/{NP}")
    if len(sys.argv) > 2:
        json.dump({"rng": list(rng)}, open(sys.argv[2], "w"))
        print(f"-> {sys.argv[2]} (RNG pool only)")
    ok = es < 1e-4 and ec < 1e-4 and et < 1e-2
    print("\nDECODE VERIFIED" if ok else "\nDECODE FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
