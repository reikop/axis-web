#!/usr/bin/env python3
"""Decode an AXIS AutoPlay replay (.ape) into per-actor movement + events.

A `.ape` is an encrypted (§2.3) stream of 76-byte records written by the replay
logger `FUN_0046c0b0`:  `[u32 tick][u32 arg][u32 0x40][64-byte payload]` — the
payload is exactly the 64-byte network packet the game sends (SPEC §4). Packet
type is `payload[1]`; the three builders decoded so far:

  TYPE 0  (builder FUN_0048d750) -- per-tick STATE SNAPSHOT, ~96% of traffic.
      [1]=0
      [4]      packed = (actor<<4) | mode/flags     -> actor = [4] >> 4
      [5],[7],[9]  int16  quantized WORLD position x,y,z  (float->int via __ftol)
      [0xb..0x12]  orientation / velocity / weapon-fire flag bytes
      [0x13..0x3f] five 9-byte weapon/turret sub-records
                   (flags + target id + 3 aim shorts each)
      -> VERIFIED: consecutive samples form smooth world-scale trajectories.
  TYPE 1  (FUN_0048d080) -- player INPUT/action event: normalised input axes
      ([17]/[21] f32, 0..1), action/fire bitmasks ([25]/[29] u16), and an RNG
      sync block ([43..55], shared table) for hit/damage determinism.
  TYPE 2  (FUN_0048d580) -- player IDENTITY/loadout: name string ([2..18]),
      actor ([22]), ~11 equipment ids ([23..33], +1 encoded), 20-byte stat block.
  TYPE 12 -- session/control (rare).

So movement IS replicated (type 0 positions), with input+RNG events (type 1)
layered on top — a state-replication model, not pure lockstep.

    python -I parse_ape.py <file.ape> [out.json]
"""
import sys
import struct

REC = 76
PAY = 12
POS_SCALE = 10           # type-0 coords are int16; apply side (FUN_0048bca0) does *10.0


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
    for o in range(0, min(len(plain), 4096)):
        if all(o + k * REC + REC <= len(plain) and
               struct.unpack_from("<I", plain, o + k * REC + 8)[0] == 0x40
               for k in range(3)):
            return o
    return None


def parse(raw):
    plain = decrypt(raw)
    start = find_start(plain)
    if start is None:
        return {"records": 0, "types": {}, "tracks": {}, "names": {}}
    types, tracks, names = {}, {}, {}
    nrec = 0
    o = start
    while o + REC <= len(plain):
        if struct.unpack_from("<I", plain, o + 8)[0] != 0x40:
            break
        tick = struct.unpack_from("<I", plain, o)[0]
        pay = plain[o + PAY:o + PAY + 64]
        nrec += 1
        t = pay[1]
        types[t] = types.get(t, 0) + 1
        if t == 0:                                   # state snapshot -> trajectory
            actor = pay[4] >> 4
            x, y, z = struct.unpack_from("<hhh", pay, 5)
            tracks.setdefault(actor, []).append(
                [tick, x * POS_SCALE, y * POS_SCALE, z * POS_SCALE])
        elif t == 2:                                 # identity -> name
            raw_name = pay[2:19].split(b"\x00")[0]
            try:
                nm = raw_name.decode("cp949", "replace")
            except Exception:
                nm = ""
            if nm:
                names[pay[22] if pay[22] < 16 else 0] = nm
        o += REC
    return {"records": nrec, "start": start, "types": types,
            "tracks": tracks, "names": names}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    raw = open(sys.argv[1], "rb").read()
    r = parse(raw)
    print(f"records={r['records']} start={r.get('start')}")
    print(f"packet types (payload[1]): {dict(sorted(r['types'].items()))}  "
          f"[0=state/pos 1=input 2=identity 12=session]")
    if r["names"]:
        print(f"names: {r['names']}")
    for a in sorted(r["tracks"]):
        pts = r["tracks"][a]
        xs = [p[1] for p in pts]
        ys = [p[2] for p in pts]
        zs = [p[3] for p in pts]
        print(f"  actor {a}: {len(pts):4d} positions  "
              f"x[{min(xs):6d},{max(xs):6d}] y[{min(ys):6d},{max(ys):6d}] z[{min(zs):6d},{max(zs):6d}]")
    if len(sys.argv) > 2:
        import json
        json.dump(r, open(sys.argv[2], "w"))
        print(f"-> {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
