#!/usr/bin/env python3
"""Draws the item icons (and a few block faces) into Resources/Textures/Blocks as
16x16 PNGs, from the little pixel maps below. Run from the repository root:
python3 tools/make_item_textures.py

Each map is 16 rows of 16 characters; each character is a colour from its palette
('.' is transparent). Small random shading is added to filled pixels so the flat
colours don't look like plastic, the same for every run (seeded)."""
import os, random, struct, zlib

OUT = os.path.join(os.path.dirname(__file__), "..", "Resources", "Textures", "Blocks")


def write_png(path, pixels):
    """pixels: 16 rows of 16 (r, g, b, a)."""
    height, width = len(pixels), len(pixels[0])
    raw = b"".join(b"\x00" + b"".join(struct.pack("BBBB", *p) for p in row) for row in pixels)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def draw(name, art, palette, jitter=10, seed=1):
    rows = [r for r in art.strip("\n").split("\n")]
    assert len(rows) == 16 and all(len(r) == 16 for r in rows), name
    rng = random.Random(seed)
    pixels = []
    for row in rows:
        out = []
        for c in row:
            if c == ".":
                out.append((0, 0, 0, 0))
                continue
            r, g, b = palette[c]
            d = rng.randint(-jitter, jitter)
            out.append((max(0, min(255, r + d)), max(0, min(255, g + d)), max(0, min(255, b + d)), 255))
        pixels.append(out)
    os.makedirs(OUT, exist_ok=True)
    write_png(os.path.join(OUT, name + ".png"), pixels)


MEAT = {"o": (60, 20, 18), "r": (178, 52, 48), "R": (208, 78, 70), "f": (238, 220, 205), "b": (232, 226, 214)}
COOKED = {"o": (45, 25, 12), "r": (122, 66, 32), "R": (152, 92, 46), "f": (196, 150, 96), "b": (226, 214, 190)}
VENISON = """
................
................
......oooo......
....oorRRroo....
...orRRrRRRro...
..orRrfRRrRRro..
..orRRffRRrRro..
.orRRRRfRRRRro..
.orrRRRRRrRro...
.orrrRRRRRro....
..orrrrRrro.bb..
...oorrrro.bbbb.
.....oooo.bbbbb.
...........bbb..
................
................
"""


def main():
    draw("raw_venison", VENISON, MEAT, seed=11)
    draw("cooked_venison", VENISON, COOKED, seed=12)


if __name__ == "__main__":
    main()
