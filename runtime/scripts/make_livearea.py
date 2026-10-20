# SPDX-License-Identifier: AGPL-3.0-only
"""Generate the Vita LiveArea images of the game (sce_sys/) and of the updater (updater/sce_sys/).

The game's bubble shows PokeRogue's rocky coast arena backdrop by Vari, CC-BY-NC-SA-4.0 (see
sce_sys/README.md): it is downloaded from a pinned pokerogue-assets commit into .cache/ and checked
by hash, never committed. The text and the updater's images are our own pixel art. No logo, no
Nintendo art: Pagefault asks that the PokeRogue logo not be reused, as it would imply endorsement.

The Vita requires 8-bit palette PNGs here, so the images are written as PNG color type 3 directly.
Only the standard library is needed. Run: python3 scripts/make_livearea.py
"""
import hashlib
import os
import struct
import urllib.request
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(HERE, "..")
ARENA_URL = ("https://raw.githubusercontent.com/pagefaultgames/pokerogue-assets/"
             "009cac30c39d3615c7b2c320c3984af368871031/images/arenas/rockycoast_bg.png")
ARENA_SHA256 = "e9a130209a9a227672c6077eb23030a4c5af99bc02ae6c18740be5d692014f1e"

NIGHT = (0x1E, 0x12, 0x26)
PURPLE = (0x33, 0x21, 0x3D)     # app background
RED = (0xDA, 0x38, 0x38)        # PokeRogue red (the site's theme colour)
WHITE = (0xF5, 0xEB, 0xFA)
DARK_RED = (0x8E, 0x1F, 0x2B)   # shadow
LIGHT_PURPLE = (0x4B, 0x34, 0x5A)
GREEN = (0x6C, 0xA8, 0x5C)
LAVENDER = (0xB4, 0xA4, 0xC0)   # secondary text
PANEL = (24, 20, 36)            # the dark box behind the text, 200/255 opaque
EDGE = (214, 206, 224)

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
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    " ": ["....."] * 7,
}


class Image:
    """RGB pixels; the palette is built when saving."""

    def __init__(self, w, h, bg):
        self.w, self.h = w, h
        self.px = [bg] * (w * h)

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

    def paste(self, src, sx, sy, scale=1):
        """Fills this image from src's pixels starting at (sx, sy), each one scale x scale."""
        for y in range(self.h):
            row = (sy + y // scale) * src.w + sx
            for x in range(self.w):
                self.px[y * self.w + x] = src.px[row + x // scale]

    def panel(self, x, y, w, h):
        """A see-through dark box with a light edge one pixel inside it."""
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                c = self.px[yy * self.w + xx]
                self.px[yy * self.w + xx] = tuple((p * 200 + b * 55) // 255 for p, b in zip(PANEL, c))
        self.rect(x + 1, y + 1, w - 2, 1, EDGE)
        self.rect(x + 1, y + h - 2, w - 2, 1, EDGE)
        self.rect(x + 1, y + 1, 1, h - 2, EDGE)
        self.rect(x + w - 2, y + 1, 1, h - 2, EDGE)

    def save(self, path):
        palette = sorted(set(self.px))
        assert len(palette) <= 256, f"{path}: {len(palette)} colours"
        index = {c: i for i, c in enumerate(palette)}
        raw = b"".join(b"\x00" + bytes(index[c] for c in self.px[y * self.w:(y + 1) * self.w]) for y in range(self.h))

        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        png = b"\x89PNG\r\n\x1a\n"
        png += chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 3, 0, 0, 0))
        png += chunk(b"PLTE", b"".join(bytes(c) for c in palette))
        png += chunk(b"IDAT", zlib.compress(raw, 9))
        png += chunk(b"IEND", b"")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(png)


def read_palette_png(data):
    """Decodes a non-interlaced palette PNG (what the arena files are) into an Image."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, plte = 8, b"", []
    while pos < len(data):
        n, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            assert ctype == 3 and interlace == 0 and depth in (1, 2, 4, 8)
        elif tag == b"PLTE":
            plte = [tuple(body[i:i + 3]) for i in range(0, n, 3)]
        elif tag == b"IDAT":
            idat += body
        pos += 12 + n
    raw, stride = zlib.decompress(idat), (w * depth + 7) // 8
    img, prev = Image(w, h, None), bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):  # filter bytes step one byte: bit depths <= 8 have 1 byte per pixel at most
            a, b, c = line[i - 1] if i else 0, prev[i], prev[i - 1] if i else 0
            pred = (0, a, b, (a + b) // 2, a if abs(b - c) <= abs(a - c) and abs(b - c) <= abs(a + b - 2 * c)
                    else b if abs(a - c) <= abs(a + b - 2 * c) else c)[f]
            line[i] = (line[i] + pred) & 0xFF
        per = 8 // depth
        for x in range(w):
            v = (line[x // per] >> (8 - depth * (x % per + 1))) & ((1 << depth) - 1)
            img.px[y * w + x] = plte[v]
        prev = line
    return img


def arena():
    cache = os.path.join(RUNTIME, ".cache", "livearea", "rockycoast_bg.png")
    if not os.path.exists(cache):
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        with urllib.request.urlopen(ARENA_URL, timeout=60) as r, open(cache + ".part", "wb") as f:
            f.write(r.read())
        os.replace(cache + ".part", cache)
    with open(cache, "rb") as f:
        data = f.read()
    assert hashlib.sha256(data).hexdigest() == ARENA_SHA256, f"{cache}: unexpected file (delete it to fetch again)"
    return read_palette_png(data)


def centered(img, s, y, scale, c, shadow=None):
    img.text(s, (img.w - Image.text_width(s, scale)) // 2, y, scale, c, shadow)


def stripes(img, top, bottom):
    """Ground stripes, like the game's battle backdrops."""
    for i, y in enumerate(range(top, bottom, 6)):
        img.rect(0, y, img.w, 3, GREEN if i % 2 == 0 else LIGHT_PURPLE)


def template(root):
    with open(os.path.join(root, "livearea", "contents", "template.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<livearea style="a1" format-ver="01.00" content-rev="1">\n'
                '  <livearea-background>\n    <image>bg.png</image>\n  </livearea-background>\n'
                '  <gate>\n    <startup-image>startup.png</startup-image>\n  </gate>\n'
                '</livearea>\n')


def game(root, scene):
    """The 320x180 arena backdrop: at 3x behind the LiveArea, 1x behind the gate and the icon."""
    icon = Image(128, 128, None)
    icon.paste(scene, 96, 40)
    icon.panel(6, 6, 116, 88)
    icon.text("PR", 21, 22, 8, WHITE, shadow=DARK_RED)
    icon.rect(0, 100, 128, 28, DARK_RED)
    centered(icon, "VITA", 108, 2, WHITE)
    icon.save(os.path.join(root, "icon0.png"))

    bg = Image(840, 500, None)
    bg.paste(scene, 20, 13, scale=3)
    bg.panel(84, 96, 672, 246)
    centered(bg, "POKEROGUE", 120, 12, WHITE, shadow=DARK_RED)
    centered(bg, "FOR PS VITA", 240, 5, RED)
    centered(bg, "UNOFFICIAL FAN PORT", 300, 3, LAVENDER)
    bg.save(os.path.join(root, "livearea", "contents", "bg.png"))

    start = Image(280, 158, None)
    start.paste(scene, 20, 0)
    start.panel(4, 36, 272, 58)
    centered(start, "POKEROGUE", 46, 5, WHITE, shadow=DARK_RED)
    start.rect(0, 120, 280, 38, DARK_RED)
    centered(start, "START", 128, 3, WHITE)
    start.save(os.path.join(root, "livearea", "contents", "startup.png"))
    template(root)


def updater(root):
    """The original flat art, labelled, so the updater's bubble can't be taken for the game's."""
    icon = Image(128, 128, PURPLE)
    icon.rect(8, 8, 112, 112, DARK_RED)  # red tile with a darker edge
    icon.rect(8, 8, 108, 108, RED)
    icon.text("PR", 21, 30, 8, WHITE, shadow=DARK_RED)
    icon.rect(8, 100, 108, 16, NIGHT)
    centered(icon, "UPDATER", 104, 1, WHITE)
    icon.save(os.path.join(root, "icon0.png"))

    bg = Image(840, 500, PURPLE)
    stripes(bg, 360, 500)
    centered(bg, "POKEROGUE", 120, 12, WHITE, shadow=DARK_RED)
    centered(bg, "UPDATER", 240, 5, RED)
    centered(bg, "INSTALLS TEST BUILDS", 300, 3, LAVENDER)
    bg.save(os.path.join(root, "livearea", "contents", "bg.png"))

    start = Image(280, 158, RED)
    start.rect(0, 120, 280, 38, DARK_RED)
    centered(start, "UPDATER", 46, 5, WHITE, shadow=DARK_RED)
    centered(start, "START", 128, 3, WHITE)
    start.save(os.path.join(root, "livearea", "contents", "startup.png"))
    template(root)


def main():
    game(os.path.join(RUNTIME, "sce_sys"), arena())
    updater(os.path.join(RUNTIME, "updater", "sce_sys"))


if __name__ == "__main__":
    main()
