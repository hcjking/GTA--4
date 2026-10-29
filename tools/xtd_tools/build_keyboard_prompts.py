#!/usr/bin/env python3
"""Builds the keyboard button-prompt set: buttons_360.xtd with keycaps.

The game draws a help token's glyph with the texture of that button's name,
so this set keeps the names and puts a PC key in each picture; the glyph
hook (gta4_keyboard_prompts.cpp) then picks the glyph whose picture is the
key the PC binds to the token's action. No Wine or xcompress: textures are
encoded here (BC3), tiled with button_prompt_roundtrip's swizzle, and the
resource repacked with xtd_store's uncompressed LZX blocks.

Usage: build_keyboard_prompts.py <xtd_store binary> <output.xtd> [work dir]
"""

from __future__ import annotations

import importlib.util
import os
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
TEMPLATE = REPO / "LibertyRecompLib/private/button_prompts/xbox360/buttons_360.xtd"

# The picture each texture holds in the keyboard set. Keep in step with
# the Glyph enum in gta4_keyboard_prompts.cpp. The textures the menus' raw
# tokens name (PAD_A select, PAD_B back, PAD_UP ... PAD_RIGHT move, PAD_X /
# PAD_Y / shoulders and triggers their extra actions) hold the PC menu keys
# (gta4_input_hooks.cpp kFrontend* bindings); the help text's actions also
# use the slots its tokens rarely name (RB, Back, the "none" and two-way
# d-pad and stick pictures) for the keys those don't cover.
KEYS = {
    "a_butt": "ENTER", "b_butt": "ESC", "x_butt": "R", "y_butt": "SPACE",
    "lb_butt": "Q", "lt_butt": "RMB", "rb_butt": "H", "rt_butt": "LMB",
    "start_butt": "ESC", "back_butt": "F",
    "up_arrow": "↑", "down_arrow": "↓", "left_arrow": "←", "right_arrow": "→",
    "dpad_up": "↑", "dpad_down": "↓", "dpad_left": "←", "dpad_right": "→",
    "dpad_none": "SHIFT", "dpad_all": "W", "dpad_updown": "S", "dpad_leftright": "G",
}
for name in ("all", "up", "down", "left", "right", "updown", "leftright", "none"):
    KEYS["lstick_" + name] = "WASD"
    KEYS["rstick_" + name] = "MOUSE"
KEYS["lstick_none"] = "CTRL"
KEYS["rstick_none"] = "E"


def load_roundtrip():
    spec = importlib.util.spec_from_file_location("roundtrip", HERE / "button_prompt_roundtrip.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["roundtrip"] = module  # (its dataclasses look themselves up there)
    spec.loader.exec_module(module)
    return module


def render(label: str, width: int, height: int, work: Path) -> bytes:
    """The keycap as RGBA bytes (AppKit draws it, ffmpeg scales and unpacks)."""
    png = work / f"key_{abs(hash(label))}.png"
    subprocess.run(["osascript", "-l", "JavaScript", str(HERE / "keycap.js"), str(png), label],
                   check=True, capture_output=True)
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(png), "-vf", f"scale={width}:{height}:flags=lanczos",
                          "-f", "rawvideo", "-pix_fmt", "rgba", "-"], check=True, capture_output=True).stdout
    if len(raw) != width * height * 4:
        raise SystemExit(f"keycap {label}: {len(raw)} bytes for {width}x{height}")
    return raw


def bc3(rgba: bytes, width: int, height: int) -> bytes:
    """A plain BC3 (DXT5) encoder: per block, colour endpoints from the
    extremes along luminance, alpha from its range."""
    out = bytearray()
    for by in range(0, height, 4):
        for bx in range(0, width, 4):
            px = [rgba[((by + y) * width + bx + x) * 4:((by + y) * width + bx + x) * 4 + 4]
                  for y in range(4) for x in range(4)]
            alphas = [p[3] for p in px]
            a0, a1 = max(alphas), min(alphas)
            if a0 == a1:
                out += bytes([a0, a1]) + bytes(6)
            else:
                levels = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
                bits = 0
                for i, a in enumerate(alphas):
                    best = min(range(8), key=lambda k: abs(levels[k] - a))
                    bits |= best << (3 * i)
                out += bytes([a0, a1]) + bits.to_bytes(6, "little")
            lum = [p[0] * 3 + p[1] * 6 + p[2] for p in px]
            hi, lo = px[lum.index(max(lum))], px[lum.index(min(lum))]
            to565 = lambda p: ((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3)
            c0, c1 = to565(hi), to565(lo)
            if c0 < c1:
                c0, c1, hi, lo = c1, c0, lo, hi
            from565 = lambda c: (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
            e0, e1 = from565(c0), from565(c1)
            palette = [e0, e1, tuple((2 * a + b) // 3 for a, b in zip(e0, e1)),
                       tuple((a + 2 * b) // 3 for a, b in zip(e0, e1))]
            bits = 0
            if c0 != c1:
                for i, p in enumerate(px):
                    best = min(range(4), key=lambda k: sum((palette[k][j] - p[j]) ** 2 for j in range(3)))
                    bits |= best << (2 * i)
            out += struct.pack("<HHI", c0, c1, bits)
    return bytes(out)


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    store, output = Path(sys.argv[1]), Path(sys.argv[2])
    work = Path(sys.argv[3]) if len(sys.argv) > 3 else output.parent / "keyboard_work"
    work.mkdir(parents=True, exist_ok=True)
    roundtrip = load_roundtrip()
    unpacked = work / "unpacked.bin"
    subprocess.run([str(store), "unpack", str(TEMPLATE), str(unpacked)], check=True)
    data = bytearray(unpacked.read_bytes())
    cpu_size, _ = roundtrip.rsc_segment_sizes(TEMPLATE.read_bytes()[:16])
    records = roundtrip.parse_texture_records(data)
    for name, label in KEYS.items():
        record = records[name]
        if record.texture_type != 20:
            raise SystemExit(f"{name}: texture type {record.texture_type}, not DXT5")
        payload = bc3(render(label, record.width, record.height, work), record.width, record.height)
        tiled = roundtrip.swizzle_dxt5_payload(payload, record.width, record.height)
        at = cpu_size + record.gpu_offset
        data[at:at + len(tiled)] = tiled
        print(f"{name:18s} {record.width}x{record.height} {label}")
    patched = work / "patched.bin"
    patched.write_bytes(data)
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(store), "pack", str(TEMPLATE), str(patched), str(output)], check=True)
    print("wrote", output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
