#!/usr/bin/env python3
"""Scan an installed AXIS game directory and emit web/catalog.json describing
its archives (entry names, sizes, and PTX texture metadata) plus loose media.

Only metadata is written — never pixel/mesh bytes — so the catalog is small and
the copyrighted payloads stay on disk. See docs/SPEC.md for the formats.

    python -I build_catalog.py <game_dir> <out_json>
"""
import sys
import os
import json
import struct
import re

REC, NAME = 0x44, 0x40

# encrypted data files (see docs/SPEC.md §2.3): rol8(b ^ 0x59, 1) after fread.
# Sound.dat is encrypted from offset 0; the others keep a 4-byte plaintext version.
ENCRYPTED = {"item.dat": 4, "component.dat": 4, "sound.dat": 0}


def _dec_byte(x):
    x = (x ^ 0x59) & 0xFF
    return ((x << 1) | (x >> 7)) & 0xFF


def decrypt(buf, skip):
    return bytes(buf[:skip]) + bytes(_dec_byte(x) for x in buf[skip:])


def read_encrypted(path, skip):
    """Decrypt an obfuscated .dat and return its version, header count, and the
    distinct asset path names embedded in it (record layout varies per file)."""
    raw = open(path, "rb").read()
    plain = decrypt(raw, skip)
    ver = struct.unpack_from("<I", plain, 0)[0] if skip == 4 else None
    count = struct.unpack_from("<I", plain, 4)[0] if (skip == 4 and len(plain) >= 8) else None
    names, seen = [], set()
    for m in re.finditer(rb"[\x20-\x7e]{4,}", plain):
        s = m.group().decode("cp949", "replace").strip()
        if ("\\" in s or "/" in s) and s not in seen:
            seen.add(s)
            names.append(s)
    return {"indexed": True, "encrypted": True, "version": hex(ver) if ver else None,
            "header_count": count, "file_size": len(raw),
            "count": len(names), "entries": [{"name": n} for n in names]}


def read_toc(path):
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < 4:
        return None
    (count,) = struct.unpack_from("<I", blob, 0)
    toc_end = 4 + count * REC
    # plausibility: TOC must fit and first offset must equal toc_end
    if count <= 0 or toc_end > len(blob):
        return None
    entries = []
    for i in range(count):
        base = 4 + i * REC
        name = blob[base:base + NAME].split(b"\x00")[0].decode("cp949", "replace")
        (off,) = struct.unpack_from("<I", blob, base + NAME)
        entries.append((name, off))
    if not entries or entries[0][1] != toc_end:
        return None  # not this archive shape (e.g. the obfuscated blobs)
    out = []
    for i, (name, off) in enumerate(entries):
        end = entries[i + 1][1] if i + 1 < len(entries) else len(blob)
        rec = {"name": name, "size": end - off}
        if end - off >= 16 and blob[off:off + 4] == b"PTX\x00":
            ver, w8, h8, fmt, table, _ = struct.unpack_from("<HHHHHH", blob, off + 4)
            rec["ptx"] = {"w": w8 * 8, "h": h8 * 8,
                          "fmt": {0x81: "RGB", 0x82: "RGBA"}.get(fmt, hex(fmt)),
                          "table": table}
        out.append(rec)
    return {"count": count, "entries": out}


def list_dir(game, sub, exts):
    d = os.path.join(game, sub)
    if not os.path.isdir(d):
        return []
    items = []
    for fn in sorted(os.listdir(d)):
        if os.path.splitext(fn)[1].lower() in exts:
            items.append({"name": fn, "size": os.path.getsize(os.path.join(d, fn))})
    return items


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    game, out_json = sys.argv[1], sys.argv[2]
    cat = {"game_dir": os.path.abspath(game), "archives": {}, "media": {}}

    data_dir = os.path.join(game, "Data")
    if os.path.isdir(data_dir):
        for fn in sorted(os.listdir(data_dir)):
            if not fn.lower().endswith(".dat"):
                continue
            full = os.path.join(data_dir, fn)
            size = os.path.getsize(full)
            if fn.lower() in ENCRYPTED:
                try:
                    cat["archives"][fn] = read_encrypted(full, ENCRYPTED[fn.lower()])
                except Exception as e:
                    cat["archives"][fn] = {"indexed": False, "file_size": size,
                                           "note": f"decrypt failed: {e}"}
                continue
            toc = read_toc(full)
            if toc:
                cat["archives"][fn] = {"indexed": True, "file_size": size, **toc}
            else:
                cat["archives"][fn] = {"indexed": False, "file_size": size,
                                       "note": "binary blob (loader not yet reversed)"}

    cat["media"]["maps"] = list_dir(game, "Map", {".bfd"})
    # tag each map with its container version (0x1201 expected)
    map_dir = os.path.join(game, "Map")
    for m in cat["media"]["maps"]:
        try:
            with open(os.path.join(map_dir, m["name"]), "rb") as f:
                (ver,) = struct.unpack("<I", f.read(4))
            m["version"] = f"0x{ver:x}"
        except Exception:
            pass
    cat["media"]["music"] = list_dir(game, "Music", {".mp3"})
    cat["media"]["sound"] = list_dir(game, "Sound", {".wav"})

    with open(out_json, "w", encoding="utf-8") as f:
        json.dump(cat, f, ensure_ascii=False)
    n_arch = len(cat["archives"])
    n_tex = sum(sum(1 for e in a.get("entries", []) if "ptx" in e)
                for a in cat["archives"].values())
    print(f"catalog: {n_arch} archives, {n_tex} PTX textures, "
          f"{len(cat['media']['maps'])} maps, {len(cat['media']['music'])} music "
          f"-> {out_json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
