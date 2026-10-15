#!/usr/bin/env python3
"""Decode an AXIS AutoPlay replay (.ape) into a per-actor position timeline.

A `.ape` is an encrypted (§2.3) stream of 76-byte records
`[u32 seq][u32 ?][u32 0x40][64-byte payload]` — the same 64-byte packets the game
sends over the network (SPEC §4). Player-action packets have `payload[1] == 1`,
`payload[2] = actor`, and the position at `payload+9` (x) / `payload+13` (z).

    python -I parse_ape.py <file.ape> [out.json]
"""
import sys
import struct
import math

REC = 76
PAY = 12          # payload offset within a record


def dec(b):
    def d(x):
        x = (x ^ 0x59) & 0xFF
        return ((x << 1) | (x >> 7)) & 0xFF
    return bytes(d(x) for x in b)


def decrypt(raw):
    p = dec(raw)
    if struct.unpack_from("<I", p, 0)[0] == 0x1201:
        return p
    return bytes(raw[:4]) + dec(raw[4:])


def find_start(plain):
    # first offset where three consecutive 76-byte records each hold a 0x40 size
    # field and a plausible player/other payload — locks the record grid.
    for o in range(0, min(len(plain), 4096)):
        ok = True
        for k in range(3):
            b = o + k * REC
            if b + REC > len(plain) or struct.unpack_from("<I", plain, b + 8)[0] != 0x40:
                ok = False
                break
        if ok:
            return o
    return None


def parse(raw):
    plain = decrypt(raw)
    start = find_start(plain)
    if start is None:
        return {"records": 0, "actors": {}}
    actors = {}
    nrec = 0
    o = start
    while o + REC <= len(plain):
        if struct.unpack_from("<I", plain, o + 8)[0] != 0x40:
            break
        pay = plain[o + PAY:o + PAY + 64]
        seq = struct.unpack_from("<I", plain, o)[0]
        nrec += 1
        if pay[1] == 1 and pay[2] < 16:
            x, z = struct.unpack_from("<ff", pay, 9)
            if math.isfinite(x) and math.isfinite(z) and abs(x) < 2e5 and abs(z) < 2e5:
                actors.setdefault(pay[2], []).append([seq, round(x, 1), round(z, 1)])
        o += REC
    return {"records": nrec, "actors": actors, "start": start}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    raw = open(sys.argv[1], "rb").read()
    r = parse(raw)
    print(f"records={r['records']} start={r.get('start')} actors={sorted(r['actors'])}")
    for a in sorted(r["actors"]):
        pts = r["actors"][a]
        xs = [p[1] for p in pts]
        zs = [p[2] for p in pts]
        print(f"  actor {a}: {len(pts)} samples  x[{min(xs):.0f},{max(xs):.0f}] z[{min(zs):.0f},{max(zs):.0f}]")
    if len(sys.argv) > 2:
        import json
        json.dump(r, open(sys.argv[2], "w"))
        print(f"-> {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
