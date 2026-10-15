#!/usr/bin/env python3
"""Export AXIS Shape.dat meshes to per-shape JSON for the web mesh viewer.

Exports every shape whose .shp blob parses with full byte-consumption (currently
the 948 single-group meshes; multi-group inter-group padding is still being
pinned). See docs/SPEC.md §2.7. Output is metadata derived from game data — keep
it out of the repository.

    python -I export_shapes.py <Shape.dat> <out_dir> [--limit N]
"""
import sys
import os
import json
import struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import parse_shp

REC, NAME = 0x44, 0x40


def clean(vecs):
    return [[0.0 if (x != x or abs(x) > 1e9) else round(x, 3) for x in p] for p in vecs]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    path, out_dir = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[sys.argv.index("--limit") + 1]) if "--limit" in sys.argv else None
    os.makedirs(out_dir, exist_ok=True)
    raw = open(path, "rb").read()
    count = struct.unpack_from("<I", raw, 0)[0]
    ents = []
    for i in range(count):
        b = 4 + i * REC
        name = raw[b:b + NAME].split(b"\x00")[0].decode("cp949", "replace")
        off = struct.unpack_from("<I", raw, b + NAME)[0]
        ents.append((name, off))

    index, done = [], 0
    for i, (name, off) in enumerate(ents):
        end = ents[i + 1][1] if i + 1 < len(ents) else len(raw)
        blob = raw[off:end]
        try:
            r = parse_shp.parse(blob)
        except Exception:
            continue
        if r["consumed"] != len(blob):
            continue  # only fully byte-validated meshes
        # merge all groups into one mesh, offsetting indices per group
        pos, nrm, uv, tris = [], [], [], []
        for g in r["groups"]:
            base = len(pos)
            pos += g["pos"]; nrm += g["nrm"]; uv += g["uv"]
            tris += [(a + base, b + base, c + base) for (a, b, c) in g["tris"]]
        if not pos or not tris:
            continue
        safe = name.replace("\\", "_").replace("/", "_")
        with open(os.path.join(out_dir, safe + ".json"), "w", encoding="utf-8") as f:
            json.dump({"name": name, "pos": clean(pos), "nrm": clean(nrm),
                       "uv": clean(uv), "tris": tris}, f, allow_nan=False)
        index.append({"id": safe, "name": name, "v": len(pos), "t": len(tris),
                      "g": len(r["groups"])})
        done += 1
        if limit and done >= limit:
            break
    index.sort(key=lambda e: e["name"])
    with open(os.path.join(out_dir, "index.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False)
    print(f"exported {done} meshes -> {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
