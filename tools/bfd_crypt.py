#!/usr/bin/env python3
"""AXIS obfuscation codec — used by the encrypted data files
(`Map/*.bfd`, `*.ape`, `Data/Item.dat`, `Data/Component.dat`, `Data/Sound.dat`).

The decrypting read `FUN_0046c060` does, per byte, after a normal fread:
    b = rotate_left_8(b ^ 0x59, 1)
The transform is position-independent, so decrypting a whole buffer uniformly
equals the game's per-read decryption.

Most files keep a 4-byte PLAINTEXT version prefix (0x1201 / 0x1204) read by the
plain fread `FUN_0049de96`, then the rest is encrypted. `Sound.dat` is encrypted
from offset 0. `Data/Shape.dat`, `Skeleton.dat`, `Image*.dat` are NOT encrypted.

    python -I bfd_crypt.py <in> <out> [--full]     # --full: decrypt from offset 0
"""
import sys

XOR = 0x59


def dec_byte(x):
    x = (x ^ XOR) & 0xFF
    return ((x << 1) | (x >> 7)) & 0xFF


def decrypt(buf, skip_prefix=4):
    """Return plaintext: first `skip_prefix` bytes verbatim, rest decrypted."""
    head = bytes(buf[:skip_prefix])
    return head + bytes(dec_byte(x) for x in buf[skip_prefix:])


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if len(args) != 2:
        print(__doc__)
        return 1
    skip = 0 if "--full" in sys.argv else 4
    data = open(args[0], "rb").read()
    out = decrypt(data, skip)
    open(args[1], "wb").write(out)
    print(f"decrypted {len(data)} bytes (skip_prefix={skip}) -> {args[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
