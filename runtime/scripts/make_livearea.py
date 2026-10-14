# SPDX-License-Identifier: AGPL-3.0-only
"""Generate the Vita LiveArea images (original pixel art, no game or Nintendo assets).

The Vita requires 8-bit palette PNGs here, so images are drawn with a small palette and written as
PNG color type 3 directly. Run: python3 scripts/make_livearea.py  (writes sce_sys/)
"""
import os
import struct
import zlib

PALETTE = [
    (0x1E, 0x12, 0x26),  # 0 night purple
    (0x33, 0x21, 0x3D),  # 1 purple (app background)
    (0xDA, 0x38, 0x38),  # 2 PokeRogue red (the site's theme colour)
    (0xF5, 0xEB, 0xFA),  # 3 off-white
    (0x8E, 0x1F, 0x2B),  # 4 dark red (shadow)
    (0x4B, 0x34, 0x5A),  # 5 light purple
    (0x6C, 0xA8, 0x5C),  # 6 grass green
    (0xB4, 0xA4, 0xC0),  # 7 lavender (secondary text)
]

# 5x7 pixel font, just the characters used.
FONT = {
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "V": ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "I": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "N": ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "D": ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."],
    " ": ["....."] * 7,
}


class Image:
    def __init__(self, w, h, bg):
        self.w, self.h = w, h
        self.px = bytearray([bg]) * (w * h)

    def rect(self, x, y, w, h, c):
        for yy in range(max(0, y), min(self.h, y + h)):
            row = yy * self.w
            for xx in range(max(0, x), min(self.w, x + w)):
                self.px[row + xx] = c

    def text(self, s, x, y, scale, c, shadow=None):
        for i, ch in enumerate(s):
            glyph = FONT[ch]
            for gy, line in enumerate(glyph):
                for gx, bit in enumerate(line):
                    if bit != "#":
                        continue
                    px, py = x + (i * 6 + gx) * scale, y + gy * scale
                    if shadow is not None:
                        self.rect(px + scale // 2 or 1, py + scale // 2 or 1, scale, scale, shadow)
                    self.rect(px, py, scale, scale, c)

    @staticmethod
    def text_width(s, scale):
        return (len(s) * 6 - 1) * scale

    def save(self, path):
        raw = b"".join(b"\x00" + bytes(self.px[y * self.w:(y + 1) * self.w]) for y in range(self.h))

        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        png = b"\x89PNG\r\n\x1a\n"
        png += chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 3, 0, 0, 0))
        png += chunk(b"PLTE", b"".join(bytes(c) for c in PALETTE))
        png += chunk(b"IDAT", zlib.compress(raw, 9))
        png += chunk(b"IEND", b"")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(png)


def centered(img, s, y, scale, c, shadow=None):
    img.text(s, (img.w - Image.text_width(s, scale)) // 2, y, scale, c, shadow)


def stripes(img, top, bottom):
    """Ground stripes, like the game's battle backdrops."""
    for i, y in enumerate(range(top, bottom, 6)):
        img.rect(0, y, img.w, 3, 6 if i % 2 == 0 else 5)


def main():
    root = os.path.join(os.path.dirname(__file__), "..", "sce_sys")

    icon = Image(128, 128, 1)
    icon.rect(8, 8, 112, 112, 4)       # red tile with a darker edge
    icon.rect(8, 8, 108, 108, 2)
    icon.text("PR", 21, 30, 8, 3, shadow=4)
    icon.rect(8, 100, 108, 16, 0)
    centered(icon, "VITA", 104, 1, 3)
    icon.save(os.path.join(root, "icon0.png"))

    bg = Image(840, 500, 1)
    stripes(bg, 360, 500)
    centered(bg, "POKEROGUE", 120, 12, 3, shadow=4)
    centered(bg, "FOR PS VITA", 240, 5, 2)
    centered(bg, "UNOFFICIAL FAN PORT", 300, 3, 7)
    bg.save(os.path.join(root, "livearea", "contents", "bg.png"))

    start = Image(280, 158, 2)
    start.rect(0, 120, 280, 38, 4)
    centered(start, "POKEROGUE", 46, 5, 3, shadow=4)
    centered(start, "START", 128, 3, 3)
    start.save(os.path.join(root, "livearea", "contents", "startup.png"))

    with open(os.path.join(root, "livearea", "contents", "template.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<livearea style="a1" format-ver="01.00" content-rev="1">\n'
                '  <livearea-background>\n    <image>bg.png</image>\n  </livearea-background>\n'
                '  <gate>\n    <startup-image>startup.png</startup-image>\n  </gate>\n'
                '</livearea>\n')


if __name__ == "__main__":
    main()
