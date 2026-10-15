#!/usr/bin/env python3
"""AXIS .shp mesh parser (the Shape.dat blob format, tag 0x71110300).

Reverse-engineered from FUN_004655c0. Layout (plaintext; Shape.dat is NOT encrypted):

  object header  0x4C bytes:
      +0x10 i32 groupCount      +0x20 f32[3] bbox-seed     +0x30 u32 tag (0x71110300)
  groupCount × group header, 0x80 bytes each:
      +0x34 i32 vertexCount     +0x48 i32 faceGroupCount
      +0x50 f32[6] bbox (min xyz, max xyz)   (other fields are saved pointers, ignored)
  then, per group, the data blocks in order:
      positions   vertexCount × vec3 (12B)
      normals     vertexCount × vec3 (12B)
      attr        vertexCount × 4B    (colour/flags)
      uv          vertexCount × 2×f32 (8B)
      faceGroups  faceGroupCount × 12B  {u32 ?, u32 indexCount, u32 savedPtr}
      indices     per faceGroup: indexCount × u32

    python -I parse_shp.py <Shape.dat> <offset> <size> [--json out.json]
"""
import sys
import struct

OBJ_HDR = 0x4C
GRP_HDR = 0x80
TAG = 0x71110300


def parse(blob):
    (tag,) = struct.unpack_from("<I", blob, 0x30)
    (gcount,) = struct.unpack_from("<i", blob, 0x10)
    if tag != TAG:
        raise ValueError(f"unexpected tag {tag:#x} (only new format 0x71110300 handled)")
    groups_meta = []
    for gi in range(gcount):
        g = OBJ_HDR + gi * GRP_HDR
        vc = struct.unpack_from("<i", blob, g + 0x34)[0]
        fgc = struct.unpack_from("<i", blob, g + 0x48)[0]
        bbox = struct.unpack_from("<6f", blob, g + 0x50)
        groups_meta.append((vc, fgc, bbox))

    p = OBJ_HDR + gcount * GRP_HDR
    groups = []
    for vc, fgc, bbox in groups_meta:
        pos = [struct.unpack_from("<fff", blob, p + i * 12) for i in range(vc)]
        p += vc * 12
        nrm = [struct.unpack_from("<fff", blob, p + i * 12) for i in range(vc)]
        p += vc * 12
        p += vc * 4                       # attr
        uv = [struct.unpack_from("<ff", blob, p + i * 8) for i in range(vc)]
        p += vc * 8
        # triangles follow as 3×u32; read while all three indices are valid (< vc).
        # (the face-group header fields are saved pointers on disk; this empirical
        #  reader recovers the triangle list robustly — the next group's vertex
        #  floats decode as huge ints and stop it.)
        tri = []
        while p + 12 <= len(blob):
            a, b, c = struct.unpack_from("<III", blob, p)
            if a < vc and b < vc and c < vc:
                tri.append((a, b, c))
                p += 12
            else:
                break
        groups.append({"vc": vc, "fgc": fgc, "pos": pos, "nrm": nrm, "uv": uv,
                       "tris": tri, "bbox": [round(b, 1) for b in bbox]})
    return {"tag": hex(tag), "groups": groups, "consumed": p}


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    path, off, size = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    raw = open(path, "rb").read()
    blob = raw[off:off + size]
    r = parse(blob)
    total_v = sum(g["vc"] for g in r["groups"])
    total_t = sum(len(g["tris"]) for g in r["groups"])
    print(f"tag={r['tag']} groups={len(r['groups'])} verts={total_v} tris={total_t}")
    print(f"consumed {r['consumed']} / blob {size}  match={r['consumed']==size}")
    for i, g in enumerate(r["groups"]):
        print(f"  group{i}: vc={g['vc']} tris={len(g['tris'])} bbox={g['bbox']}")
    if "--json" in sys.argv:
        import json
        out = sys.argv[sys.argv.index("--json") + 1]
        json.dump({"groups": [{"pos": g["pos"], "nrm": g["nrm"], "uv": g["uv"],
                               "tris": g["tris"]} for g in r["groups"]]},
                  open(out, "w"))
        print(f"  -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
