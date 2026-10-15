#!/usr/bin/env python3
"""Probe the per-bone animation keyframe region of a Skeleton.dat entry.

Each Skeleton.dat entry (see parse_skeleton.py / docs/SPEC.md §2.10) is:
    header  ->  bone array (bind pose, bc * 0x70)  ->  ANIMATION REGION

This tool inspects that animation region. Verified facts (cross-checked against
the bind pose, which the tool uses as ground truth):
  * header @0x1c = keyframe/sample count (varies per rig: 30, 34, 6, 24 ...);
    the region's size scales with it, so the format is variable-length.
  * header @0x18 = clip/animation count or flags; @0x20 = constant 3300.
  * The region holds, per bone, axis-angle ROTATION keyframes (angle + unit
    axis, 4 floats each); keyframe 0 equals the bind-pose rotation exactly.
  * The header block carries ROOT TRANSLATION keyframes (the root joint's xyz
    moving frame to frame).
  * int32 markers 1650 and 3300 (= 2*1650) delimit sub-structure (key times /
    indices). Their exact time base, the per-key record stride, and the
    rotation/position track interleave are NOT yet pinned down from the file
    alone -- that needs the runtime animation sampler reversed (or a dynamic
    trace). This tool deliberately reports structure, not a guessed playback.

Run isolated:
    python -I probe_skeleton_anim.py <Skeleton.dat> <name-substr>
"""
import sys
import struct
import math

REC = 0x44
NAME = 0x40
STRIDE = 0x70


def _toc(blob):
    (count,) = struct.unpack_from("<I", blob, 0)
    ent = []
    for i in range(count):
        base = 4 + i * REC
        nm = blob[base:base + NAME].split(b"\x00")[0].decode("cp949", "replace")
        ent.append((nm, struct.unpack_from("<I", blob, base + NAME)[0]))
    out = []
    for i, (nm, off) in enumerate(ent):
        end = ent[i + 1][1] if i + 1 < len(ent) else len(blob)
        out.append((nm, off, end - off))
    return out


def _f(d, o):
    return struct.unpack_from("<f", d, o)[0]


def _i(d, o):
    return struct.unpack_from("<i", d, o)[0]


def probe(path, key):
    blob = open(path, "rb").read()
    hits = [x for x in _toc(blob) if key.upper() in x[0].upper()]
    if not hits:
        print(f"no skeleton matching {key!r}")
        return 1
    nm, off, size = hits[0]
    d = blob[off:off + size]
    bc = _i(d, 0x10)

    binds = []
    for b in range(bc):
        bo = 0x24 + b * STRIDE
        nz = 0
        while nz < NAME and d[bo + nz]:
            nz += 1
        name = d[bo:bo + nz].decode("cp949", "replace")
        rot = (_f(d, bo + 0x50), _f(d, bo + 0x54), _f(d, bo + 0x58), _f(d, bo + 0x5c))
        binds.append((name, rot))

    bone_end = 0x24 + bc * STRIDE
    region = d[bone_end:]
    key_count = _i(d, 0x1c)
    print(f"{nm!r}  bones={bc}  key_count(@0x1c)={key_count}  clips?(@0x18)={_i(d, 0x18)}  @0x20={_i(d, 0x20)}")
    print(f"animation region: {len(region)} bytes (abs 0x{bone_end:x})")

    def match_rot(a, b, c, e):
        for n, r in binds:
            if abs(a - r[0]) < 0.01 and abs(b - r[1]) < 0.01 and abs(c - r[2]) < 0.01 and abs(e - r[3]) < 0.01:
                return n
        return None

    anchors = []
    N = len(region) // 4
    for w in range(N):
        o = w * 4
        if o + 16 > len(region):
            break
        a, b, c, e = _f(region, o), _f(region, o + 4), _f(region, o + 8), _f(region, o + 12)
        n = match_rot(a, b, c, e)
        if n:
            anchors.append((o, n))
    print("\nbind-pose rotation anchors inside the region (frame-0 keyframe per bone):")
    prev = None
    for o, n in anchors:
        stride = f"  (+0x{o - prev:x} from prev)" if prev is not None else ""
        print(f"  +0x{o:04x}  {n}.rot{stride}")
        prev = o
    if len(anchors) >= 2:
        deltas = [anchors[i + 1][0] - anchors[i][0] for i in range(len(anchors) - 1)]
        if len(set(deltas)) == 1:
            print(f"\n=> uniform per-bone block stride: 0x{deltas[0]:x} ({deltas[0]} bytes = {deltas[0] // 4} floats)")
        else:
            print(f"\n=> per-bone block strides vary: {[hex(x) for x in deltas]}")
    return 0


def main():
    if len(sys.argv) < 3:
        print("usage: python -I probe_skeleton_anim.py <Skeleton.dat> <name-substr>")
        return 1
    return probe(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    sys.exit(main())
