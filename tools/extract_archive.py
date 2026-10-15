#!/usr/bin/env python3
"""Extract every entry from an AXIS indexed .dat archive to a directory, and
summarise PTX texture headers.

Archive format and PTX header: see docs/SPEC.md (derived from AXIS.EXE
FUN_004660a0 / FUN_00454510 / FUN_0046df80).

Run isolated:
    python -I extract_archive.py <file.dat> [out_dir] [--manifest manifest.json]

With no out_dir it only prints a summary + writes the manifest (no files dumped).
Never commit extracted output — it is derived from copyrighted game data.
"""
import sys
import os
import json
import struct

REC = 0x44
NAME = 0x40


def read_toc(blob):
    (count,) = struct.unpack_from("<I", blob, 0)
    entries = []
    for i in range(count):
        base = 4 + i * REC
        name = blob[base:base + NAME].split(b"\x00")[0].decode("cp949", "replace")
        (off,) = struct.unpack_from("<I", blob, base + NAME)
        entries.append((name, off))
    out = []
    for i, (name, off) in enumerate(entries):
        end = entries[i + 1][1] if i + 1 < len(entries) else len(blob)
        out.append((name, off, end - off))
    return count, out


def parse_ptx(data):
    """Return dict of PTX header fields, or None if not a PTX blob."""
    if len(data) < 16 or data[:4] != b"PTX\x00":
        return None
    ver, w8, h8, fmt, table, _pad = struct.unpack_from("<HHHHHH", data, 4)
    return {
        "version": ver,
        "width": w8 * 8,
        "height": h8 * 8,
        "format": "RGBA" if fmt == 0x82 else "RGB" if fmt == 0x81 else f"0x{fmt:x}",
        "planes": 4 if fmt == 0x82 else 3 if fmt == 0x81 else None,
        "table": table,
    }


def safe_rel(name):
    # archive paths use backslashes; keep them as sub-dirs, strip traversal
    parts = [p for p in name.replace("\\", "/").split("/") if p not in ("", ".", "..")]
    return os.path.join(*parts) if parts else "entry"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    manifest_path = None
    if "--manifest" in sys.argv:
        manifest_path = sys.argv[sys.argv.index("--manifest") + 1]
    if not args:
        print(__doc__)
        return 1
    path = args[0]
    out_dir = args[1] if len(args) > 1 else None

    with open(path, "rb") as f:
        blob = f.read()
    count, entries = read_toc(blob)

    manifest = {"file": os.path.basename(path), "count": count, "entries": []}
    ptx_count = 0
    for name, off, size in entries:
        rec = {"name": name, "offset": off, "size": size}
        ptx = parse_ptx(blob[off:off + 16])
        if ptx:
            ptx_count += 1
            rec["ptx"] = ptx
        manifest["entries"].append(rec)
        if out_dir:
            dst = os.path.join(out_dir, safe_rel(name)) + ".bin"
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as g:
                g.write(blob[off:off + size])

    print(f"{manifest['file']}: {count} entries, {ptx_count} PTX textures"
          + (f", extracted to {out_dir}" if out_dir else " (summary only)"))
    if ptx_count:
        dims = {}
        for e in manifest["entries"]:
            if "ptx" in e:
                k = f"{e['ptx']['width']}x{e['ptx']['height']} {e['ptx']['format']}"
                dims[k] = dims.get(k, 0) + 1
        print("  PTX sizes: " + ", ".join(f"{k}:{v}" for k, v in sorted(dims.items())))
    if manifest_path:
        with open(manifest_path, "w", encoding="utf-8") as g:
            json.dump(manifest, g, ensure_ascii=False, indent=0)
        print(f"  manifest -> {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
