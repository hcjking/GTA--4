#!/usr/bin/env python3
"""Extracts one DXT5 texture of an unpacked Xbox 360 texture dictionary as a
.dds file (untiled, endian swap undone), for viewing or as the starting point
of a replacement.

Usage: xtd_extract_dxt5.py <original.xtd> <unpacked.bin> <texture name> <out.dds>
(unpacked.bin from `xtd_store unpack`)
"""

import importlib.util
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def load_roundtrip():
    spec = importlib.util.spec_from_file_location("roundtrip", HERE / "button_prompt_roundtrip.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["roundtrip"] = module  # (its dataclasses look themselves up there)
    spec.loader.exec_module(module)
    return module


def dds_header(width: int, height: int) -> bytes:
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000  # caps, height, width, pixel format, linear size
    pixel_format = struct.pack("<II4s5I", 32, 0x4, b"DXT5", 0, 0, 0, 0, 0)
    header = struct.pack("<7I", 124, flags, height, width, width * height, 0, 1) + b"\0" * 44 + pixel_format
    header += struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    return b"DDS " + header


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    original, unpacked, name, out = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], Path(sys.argv[4])
    roundtrip = load_roundtrip()
    data = bytearray(unpacked.read_bytes())
    # (any dictionary's records, not only the button prompts' set)
    names = []
    for marker in data.split(b"pack:/")[1:]:
        names.append(marker.split(b"\0")[0].decode("ascii", "replace").removesuffix(".dds"))
    roundtrip.TEXTURE_NAMES = names
    records = roundtrip.parse_texture_records(data)
    record = records[name]
    if record.texture_type != 20:
        raise SystemExit(f"{name}: texture type {record.texture_type}, not DXT5")
    cpu_size, _ = roundtrip.rsc_segment_sizes(original.read_bytes()[:16])
    base = cpu_size + record.gpu_offset
    block_width, block_height = record.width // 4, record.height // 4
    linear = bytearray(block_width * block_height * 16)
    for y in range(block_height):
        for x in range(block_width):
            at = base + roundtrip.xg_address_2d_tiled_offset(x, y, block_width, 16) * 16
            block = bytearray(data[at:at + 16])
            for i in range(0, 16, 2):
                block[i], block[i + 1] = block[i + 1], block[i]
            linear[(x + y * block_width) * 16:(x + y * block_width) * 16 + 16] = block
    out.write_bytes(dds_header(record.width, record.height) + linear)
    print("wrote", out, record.width, "x", record.height)
    return 0


if __name__ == "__main__":
    sys.exit(main())
