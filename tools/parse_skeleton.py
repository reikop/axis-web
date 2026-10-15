#!/usr/bin/env python3
"""AXIS Skeleton.dat parser + exporter (bind-pose skeletons).

Skeleton.dat is the plaintext indexed archive (see parse_archive.py). Each entry
is one skeleton. Layout confirmed empirically + from `class SkeletonManager`
(AXIS.EXE 0x4c6440); see docs/SPEC.md §2.10:

  entry header: i32 @0x10 = bone count ; bone records start at +0x24
  bone record, stride 0x70 (112) bytes:
      char name[0x40]         NUL-terminated joint name (e.g. "LARM01")
      i32  parent  @0x40      parent bone index, -1 for a root
      f32  tx,ty,tz @0x44     local translation from parent (real bone lengths)
      f32  angle   @0x50      rotation angle, radians
      f32  ax,ay,az @0x54     unit rotation axis (|axis| ~= 1.0)      (axis-angle)
      (+0x60.. : runtime-only fields / saved pointers, ignored)

Decode verified by left/right mirror symmetry: both arms give bone lengths
125.9 / 75.6 / 264.8; both legs' foot nodes land on one Y plane. Not a guess.

Run isolated (untrusted game data is an argument, never imported):
    python -I parse_skeleton.py <Skeleton.dat>                 # list + summary
    python -I parse_skeleton.py <Skeleton.dat> <out_dir>       # export JSON
"""
import sys
import os
import json
import struct
import math

REC = 0x44        # TOC record
NAME = 0x40       # TOC name field
BONE_STRIDE = 0x70
BONE_FIRST = 0x24


def _toc(blob):
    (count,) = struct.unpack_from("<I", blob, 0)
    ent = []
    for i in range(count):
        base = 4 + i * REC
        nm = blob[base:base + NAME].split(b"\x00")[0].decode("cp949", "replace")
        (off,) = struct.unpack_from("<I", blob, base + NAME)
        ent.append((nm, off))
    sized = []
    for i, (nm, off) in enumerate(ent):
        end = ent[i + 1][1] if i + 1 < len(ent) else len(blob)
        sized.append((nm, off, end - off))
    return sized


def parse_skeleton(data):
    """data = one entry's bytes -> {bones:[{name,parent,t:[x,y,z],angle,axis:[x,y,z]}]}."""
    (bone_count,) = struct.unpack_from("<i", data, 0x10)
    bones = []
    for b in range(bone_count):
        bo = BONE_FIRST + b * BONE_STRIDE
        if bo + BONE_STRIDE > len(data):
            break
        nz = 0
        while nz < NAME and data[bo + nz] != 0:
            nz += 1
        name = data[bo:bo + nz].decode("cp949", "replace")
        parent = struct.unpack_from("<i", data, bo + 0x40)[0]
        tx, ty, tz = struct.unpack_from("<fff", data, bo + 0x44)
        ang = struct.unpack_from("<f", data, bo + 0x50)[0]
        ax, ay, az = struct.unpack_from("<fff", data, bo + 0x54)
        bones.append({"name": name, "parent": parent,
                      "t": [tx, ty, tz], "angle": ang, "axis": [ax, ay, az]})
    return {"boneCount": bone_count, "bones": bones}


# --- world-space joint positions (bind pose), for validation / fallback JSON ---
def _axisangle(th, ax, ay, az):
    n = math.sqrt(ax * ax + ay * ay + az * az) or 1.0
    ax, ay, az = ax / n, ay / n, az / n
    c, s, t = math.cos(th), math.sin(th), 1 - math.cos(th)
    return [t*ax*ax+c, t*ax*ay-s*az, t*ax*az+s*ay, 0,
            t*ax*ay+s*az, t*ay*ay+c, t*ay*az-s*ax, 0,
            t*ax*az-s*ay, t*ay*az+s*ax, t*az*az+c, 0,
            0, 0, 0, 1]


def _mul(a, b):
    r = [0.0] * 16
    for i in range(4):
        for j in range(4):
            r[i*4+j] = a[i*4]*b[j] + a[i*4+1]*b[4+j] + a[i*4+2]*b[8+j] + a[i*4+3]*b[12+j]
    return r


def world_joints(sk):
    bones = sk["bones"]
    world = [None] * len(bones)

    def comp(i):
        if world[i] is not None:
            return world[i]
        b = bones[i]
        tl = [1, 0, 0, b["t"][0], 0, 1, 0, b["t"][1], 0, 0, 1, b["t"][2], 0, 0, 0, 1]
        local = _mul(tl, _axisangle(b["angle"], *b["axis"]))
        world[i] = local if b["parent"] < 0 or b["parent"] >= len(bones) \
            else _mul(comp(b["parent"]), local)
        return world[i]
    return [[comp(i)[3], comp(i)[7], comp(i)[11]] for i in range(len(bones))]


def main():
    if len(sys.argv) < 2:
        print("usage: python -I parse_skeleton.py <Skeleton.dat> [out_dir]")
        return 1
    path = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else None
    with open(path, "rb") as f:
        blob = f.read()
    sized = _toc(blob)
    print(f"{path}: {len(sized)} skeletons")

    if not out_dir:
        for nm, off, size in sized[:12]:
            sk = parse_skeleton(blob[off:off + size])
            roots = sum(1 for b in sk["bones"] if b["parent"] < 0)
            names = ",".join(b["name"] for b in sk["bones"][:6])
            print(f"  {nm:<34} bones={sk['boneCount']:>3} roots={roots}  {names}")
        return 0

    os.makedirs(out_dir, exist_ok=True)
    index = []
    ok = 0
    for nm, off, size in sized:
        sk = parse_skeleton(blob[off:off + size])
        if not sk["bones"]:
            continue
        sk["world"] = world_joints(sk)      # precomputed bind-pose joint positions
        safe = nm.replace("\\", "__").replace("/", "__")
        with open(os.path.join(out_dir, safe + ".json"), "w", encoding="utf-8") as f:
            json.dump({"name": nm, **sk}, f)
        index.append({"name": nm, "file": safe + ".json", "bones": sk["boneCount"]})
        ok += 1
    with open(os.path.join(out_dir, "index.json"), "w", encoding="utf-8") as f:
        json.dump(index, f)
    print(f"exported {ok} skeletons -> {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
