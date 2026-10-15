#!/usr/bin/env python3
"""Decode an AXIS AutoPlay replay (.ape) into its packet stream.

A `.ape` is an encrypted (§2.3) stream of 76-byte records written by the replay
logger `FUN_0046c0b0`:  `[u32 tick][u32 arg][u32 0x40][64-byte payload]` — the
payload is exactly the 64-byte network packet the game sends (SPEC §4).

Player-action packet layout, decoded authoritatively from the builder
`FUN_0048d080` and VALIDATED against this replay (see docs/SPEC.md §4):

    [0]      (leading byte)
    [1] = 1  packet type  (1 = player action)
    [2]      actor id (0..15)
    [3..8]   appearance / colour bytes (0xFF while the slot is inactive)
    [9..12]  f32  global aim/view A      (from 0x22f0408)
    [13..16] f32  global aim/view B      (from 0x22f040c)   -- varies 0..1
    [17..20] f32  per-actor input A      (0x1eccb6c + actor*0x8888)
    [21..24] f32  per-actor input B      (0x1eccb70 + actor*0x8888) -- varies 0..1
    [25..26] u16  action/fire bitmask 1  (one bit per 0..15 channel)
    [29..30] u16  action/fire bitmask 2
    [33..42] aim/angle shorts
    [43]     RNG stream selector (counter % 5)
    [44..55] RNG sync block  (5 draws from the shared table DAT_00a11bc8)

KEY FINDING: the four floats are small NORMALISED values (0..1), never world
coordinates (maps span tens of thousands of units). AXIS multiplayer is therefore
**input-based deterministic lockstep**: packets carry control inputs + action
bits + RNG samples so every peer runs the identical deterministic simulation —
not replicated positions. So a replay's absolute motion can only be recovered by
running that simulation, not by reading coordinates out of the packets.

    python -I parse_ape.py <file.ape> [out.json]
"""
import sys
import struct
import math

REC = 76
PAY = 12          # payload offset within a record


def _dec(b):
    def d(x):
        x = (x ^ 0x59) & 0xFF
        return ((x << 1) | (x >> 7)) & 0xFF
    return bytes(d(x) for x in b)


def decrypt(raw):
    p = _dec(raw)
    if struct.unpack_from("<I", p, 0)[0] == 0x1201:
        return p
    return bytes(raw[:4]) + _dec(raw[4:])


def find_start(plain):
    # lock the record grid: first offset where 3 consecutive records hold 0x40
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
        return {"records": 0, "types": {}, "actors": {}}
    types = {}
    actors = {}
    nrec = 0
    o = start
    while o + REC <= len(plain):
        if struct.unpack_from("<I", plain, o + 8)[0] != 0x40:
            break
        tick = struct.unpack_from("<I", plain, o)[0]
        pay = plain[o + PAY:o + PAY + 64]
        nrec += 1
        ptype = pay[1]
        types[ptype] = types.get(ptype, 0) + 1
        if ptype == 1 and pay[2] < 16:
            a = pay[2]
            inA, inB = struct.unpack_from("<ff", pay, 17)      # per-actor input vector
            m1, m2 = struct.unpack_from("<HH", pay, 25)         # action/fire bitmasks
            if all(map(math.isfinite, (inA, inB))):
                actors.setdefault(a, []).append(
                    {"tick": tick, "in": [round(inA, 3), round(inB, 3)], "act": [m1, m2]})
        o += REC
    return {"records": nrec, "start": start, "types": types, "actors": actors}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    raw = open(sys.argv[1], "rb").read()
    r = parse(raw)
    print(f"records={r['records']} start={r.get('start')}")
    print(f"packet types (payload[1]): {dict(sorted(r['types'].items()))}  "
          f"[0=idle 1=player-action 2=event 12=session]")
    for a in sorted(r["actors"]):
        s = r["actors"][a]
        acts = sum(1 for e in s if e["act"][0] or e["act"][1])
        print(f"  actor {a}: {len(s)} action packets, {acts} with action bits set")
    if len(sys.argv) > 2:
        import json
        json.dump(r, open(sys.argv[2], "w"))
        print(f"-> {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
