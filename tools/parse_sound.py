#!/usr/bin/env python3
"""Decrypt Data/Sound.dat and list the sound-effect definitions it indexes.

Sound.dat is encrypted from offset 0 with the standard AXIS cipher (§2.3:
per byte `rol8(b ^ 0x59, 1)`). Decrypted, it is the sound-definition table: a
list of entries each referencing a `Sound\\...\\*.wav` clip (the clips themselves
are loose standard WAV files under the install's `Sound/` tree; music is loose
MP3 under `Music/`). This tool recovers the referenced clip paths.

    python -I parse_sound.py <Sound.dat> [out.json]
"""
import sys
import re
import json


def decrypt(raw):
    def d(x):
        x = (x ^ 0x59) & 0xFF
        return ((x << 1) | (x >> 7)) & 0xFF
    return bytes(d(x) for x in raw)


def sound_paths(plain):
    # clip references look like  Sound\<cat>\<name>.wav  (cp949/ascii)
    refs = re.findall(rb'(?:Sound|sound)\\[^\x00]*?\.wav', plain)
    out, seen = [], set()
    for r in refs:
        s = r.decode('cp949', 'replace').replace('\\', '/')
        if s not in seen:
            seen.add(s)
            out.append(s)
    return out


def main():
    if len(sys.argv) < 2:
        print("usage: python -I parse_sound.py <Sound.dat> [out.json]")
        return 1
    raw = open(sys.argv[1], "rb").read()
    plain = decrypt(raw)
    paths = sound_paths(plain)
    print(f"Sound.dat: {len(raw)} bytes, {len(paths)} clip references")
    cats = {}
    for p in paths:
        c = p.split('/')[1] if p.count('/') >= 2 else '(root)'
        cats[c] = cats.get(c, 0) + 1
    print("by category:", dict(sorted(cats.items(), key=lambda x: -x[1])))
    for p in paths[:20]:
        print("  ", p)
    if len(sys.argv) > 2:
        json.dump({"clips": paths}, open(sys.argv[2], "w"))
        print(f"-> {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
