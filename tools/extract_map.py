#!/usr/bin/env python3
"""Decrypt an AXIS .bfd map and extract its object-placement list to JSON.

Layout (decrypted; see docs/SPEC.md §2.3/§2.6):
    u32  version (0x1201)
    u32  section_size  (== count*0xB0 + 4)
    u32  count
    count × 0xB0 (176-byte) object records:
        +0x00  char   shape path name (NUL-terminated, cp949)
        +0x8c  f32[3] position (x, y, z)   [confirmed by spatial spread]
        (other floats: orientation/scale — not yet identified)

The object meshes themselves live in Shape.dat (mesh format still to be decoded),
so this is the map's spatial *layout*, not yet its rendered geometry.

    python -I extract_map.py <map.bfd> <out.json>
"""
import sys
import os
import struct

REC = 0xB0
NAME = 0x88
POS = 0x8C


def _dec(x):
    x = (x ^ 0x59) & 0xFF
    return ((x << 1) | (x >> 7)) & 0xFF


def decrypt(buf):
    return bytes(buf[:4]) + bytes(_dec(x) for x in buf[4:])


def extract(path):
    plain = decrypt(open(path, "rb").read())
    ver = struct.unpack_from("<I", plain, 0)[0]
    section = struct.unpack_from("<I", plain, 4)[0]
    count = struct.unpack_from("<I", plain, 8)[0]
    layout_ok = (section == count * REC + 4)
    objs = []
    base = 0xC
    for i in range(count):
        o = base + i * REC
        if o + REC > len(plain):
            break
        name = plain[o:o + NAME].split(b"\x00")[0].decode("cp949", "replace")
        x, y, z = struct.unpack_from("<fff", plain, o + POS)
        if any(v != v or abs(v) > 1e7 for v in (x, y, z)):
            x = y = z = 0.0
        objs.append({"name": name, "pos": [round(x, 2), round(y, 2), round(z, 2)]})
    return {"map": os.path.splitext(os.path.basename(path))[0],
            "version": hex(ver), "count": count, "layout_ok": layout_ok,
            "objects": objs}


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    import json
    data = extract(sys.argv[1])
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False)
    print(f"{data['map']}: {len(data['objects'])} objects, "
          f"layout_ok={data['layout_ok']} -> {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
