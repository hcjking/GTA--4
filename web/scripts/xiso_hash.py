#!/usr/bin/env python3
"""SHA-256 of files inside an Xbox 360 disc image (XDVDFS / GDFX).

Usage: xiso_hash.py <image.iso> <name> [<name> ...]   (names at the disc root)
"""

import hashlib
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Where the game partition starts for the common disc layouts (XGD2, XGD3, plain).
PARTITION_OFFSETS = [0xFD90000, 0x2080000, 0x18300000, 0]


def find_partition(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    raise SystemExit("no XDVDFS volume descriptor found")


def read_directory(f, base, sector, size):
    """Yields (name, start sector, size, attributes) for one directory table."""
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    pending = [0]
    seen = set()
    while pending:
        offset = pending.pop()
        if offset in seen or offset + 14 > len(data):
            continue
        seen.add(offset)
        left, right, start, length, attrs, name_len = struct.unpack_from("<HHIIBB", data, offset)
        if left == 0xFFFF and right == 0xFFFF:
            continue
        name = data[offset + 14 : offset + 14 + name_len].decode("latin-1")
        yield name, start, length, attrs
        if left:
            pending.append(left * 4)
        if right:
            pending.append(right * 4)


def main():
    path, names = sys.argv[1], [n.lower() for n in sys.argv[2:]]
    with open(path, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        entries = {n.lower(): (s, l) for n, s, l, _ in read_directory(f, base, root_sector, root_size)}
        for name in names:
            if name not in entries:
                print(f"{name}: not found")
                continue
            start, length = entries[name]
            f.seek(base + start * SECTOR)
            digest = hashlib.sha256()
            remaining = length
            while remaining:
                chunk = f.read(min(remaining, 1 << 20))
                digest.update(chunk)
                remaining -= len(chunk)
            print(f"{name} {length} {digest.hexdigest()}")


if __name__ == "__main__":
    main()
