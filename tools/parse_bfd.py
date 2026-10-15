#!/usr/bin/env python3
"""Decrypt an AXIS .bfd map (or any file using the same codec) and inspect it:
print the version prefix and the embedded ASCII name strings (shape/object paths)
with their offsets. Full terrain parsing is WIP — see docs/SPEC.md §2.5; the
field layout is still being pinned against decrypted bytes.

    python -I parse_bfd.py <map.bfd> [--full] [--save out.bin]
"""
import sys
import os
import struct
import re

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bfd_crypt import decrypt


def ascii_runs(buf, minlen=4):
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minlen, buf):
        yield m.start(), m.group().decode("ascii", "replace")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if not args:
        print(__doc__)
        return 1
    skip = 0 if "--full" in sys.argv else 4
    data = open(args[0], "rb").read()
    plain = decrypt(data, skip)

    print(f"file   : {args[0]}  ({len(data)} bytes)")
    if skip == 4:
        print(f"version: {struct.unpack_from('<I', plain, 0)[0]:#x}")
    print(f"u32[1] : {struct.unpack_from('<I', plain, 4)[0]}")
    print(f"u32[2] : {struct.unpack_from('<I', plain, 8)[0]}")

    runs = [(o, s) for o, s in ascii_runs(plain, 5)]
    print(f"\nembedded names ({len(runs)}):")
    for o, s in runs[:60]:
        print(f"  @{o:#07x}  {s}")
    if len(runs) > 60:
        print(f"  … +{len(runs) - 60} more")

    if "--save" in sys.argv:
        out = sys.argv[sys.argv.index("--save") + 1]
        open(out, "wb").write(plain)
        print(f"\ndecrypted -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
