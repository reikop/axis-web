#!/usr/bin/env python3
"""AXIS .dat archive index parser.

Format (confirmed from AXIS.EXE FUN_004660a0, the Shape.dat loader):
    [u32 count]
    count * record, each 0x44 (68) bytes:
        char name[0x40]   # NUL-terminated path, may hold extra NUL-separated labels
        u32  data_offset  # offset into the file where this entry's data begins
    <data blob follows the TOC>

Entry data size is inferred as next_offset - this_offset (last entry runs to EOF).

Run isolated (untrusted game data is passed as an argument, not imported):
    python -I parse_archive.py <path-to.dat>
"""
import sys
import struct

REC = 0x44        # 68-byte record
NAME = 0x40       # 64-byte name field


def parse(path):
    with open(path, "rb") as f:
        blob = f.read()
    (count,) = struct.unpack_from("<I", blob, 0)
    toc_end = 4 + count * REC
    entries = []
    for i in range(count):
        base = 4 + i * REC
        raw = blob[base:base + NAME]
        # primary name = bytes up to first NUL; collect extra NUL-separated labels too
        parts = [p.decode("cp949", "replace") for p in raw.split(b"\x00") if p]
        (offset,) = struct.unpack_from("<I", blob, base + NAME)
        entries.append((parts[0] if parts else "", offset, parts[1:]))

    # infer sizes from successive offsets
    sized = []
    for i, (name, off, extra) in enumerate(entries):
        end = entries[i + 1][1] if i + 1 < len(entries) else len(blob)
        sized.append((name, off, end - off, extra))
    return count, toc_end, len(blob), sized


def main():
    if len(sys.argv) != 2:
        print("usage: python -I parse_archive.py <file.dat>")
        return 1
    path = sys.argv[1]
    count, toc_end, filesize, sized = parse(path)
    print(f"file        : {path}")
    print(f"entries     : {count}")
    print(f"TOC size    : {toc_end} bytes (4 + {count}*{REC})")
    print(f"file size   : {filesize} bytes")
    first_off = sized[0][1] if sized else 0
    print(f"1st data off: {first_off}  (TOC_end={toc_end}, match={first_off == toc_end})")
    # sanity: offsets monotonic non-decreasing and within file?
    offs = [o for _, o, _, _ in sized]
    mono = all(offs[i] <= offs[i + 1] for i in range(len(offs) - 1))
    in_range = all(toc_end <= o <= filesize for o in offs)
    print(f"offsets     : monotonic={mono} in_range={in_range}")
    print("\n-- first 15 entries (name | offset | size | extra-labels) --")
    for name, off, size, extra in sized[:15]:
        ex = (" " + " ".join(extra)) if extra else ""
        print(f"  {name:<28} @{off:>9} {size:>8}B{ex}")
    print("\n-- 5 largest entries --")
    for name, off, size, extra in sorted(sized, key=lambda e: -e[2])[:5]:
        print(f"  {name:<28} {size:>10}B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
