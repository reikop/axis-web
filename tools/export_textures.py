#!/usr/bin/env python3
"""Decode AXIS textures (Image*.dat PTX entries) to PNG for the web gallery.

Uses ptx_decode (baseline-JPEG). Output is derived from copyrighted game data —
keep it out of the repository.

    python -I export_textures.py <game_dir> <out_dir> [--limit N]
"""
import sys
import os
import json
import struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ptx_decode

REC, NAME = 0x44, 0x40


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 2:
        print(__doc__)
        return 1
    game, out_dir = args[0], args[1]
    limit = int(sys.argv[sys.argv.index("--limit") + 1]) if "--limit" in sys.argv else None
    os.makedirs(out_dir, exist_ok=True)
    exe = open(os.path.join(game, "AXIS.EXE"), "rb").read()
    data_dir = os.path.join(game, "Data")

    index, done = [], 0
    for fn in sorted(os.listdir(data_dir)):
        if not (fn.lower().startswith("image") and fn.lower().endswith(".dat")):
            continue
        raw = open(os.path.join(data_dir, fn), "rb").read()
        count = struct.unpack_from("<I", raw, 0)[0]
        ents = []
        for i in range(count):
            b = 4 + i * REC
            name = raw[b:b + NAME].split(b"\x00")[0].decode("cp949", "replace")
            off = struct.unpack_from("<I", raw, b + NAME)[0]
            ents.append((name, off))
        for i, (name, off) in enumerate(ents):
            end = ents[i + 1][1] if i + 1 < len(ents) else len(raw)
            blob = raw[off:end]
            if blob[:4] != b"PTX\x00":
                continue
            try:
                w, h, rgba = ptx_decode.decode(blob, exe)
            except Exception:
                continue
            safe = (fn[:-4] + "_" + name).replace("\\", "_").replace("/", "_")
            ptx_decode.write_png(os.path.join(out_dir, safe + ".png"), w, h, rgba)
            index.append({"id": safe, "name": name, "src": fn, "w": w, "h": h})
            done += 1
            if limit and done >= limit:
                break
        if limit and done >= limit:
            break
    index.sort(key=lambda e: e["name"])
    with open(os.path.join(out_dir, "index.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False)
    print(f"decoded {done} textures -> {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
