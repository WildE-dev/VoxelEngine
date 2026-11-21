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


SWORD = """
................
.............oo.
............oso.
...........osso.
..........osso..
.........osso...
........osso....
.......osso.....
..oo..osso......
..owoosso.......
...owwso........
....owwo........
...ohwwo........
..ohho.oo.......
..oho...........
...o............
"""
SWORD_COLOURS = {"o": (40, 40, 44), "s": (178, 182, 188), "w": (120, 84, 46), "h": (96, 66, 36)}

HIDE = """
................
...oo......oo...
..ottoooooottoo.
..otTttTttTttto.
...otTttttTtto..
...otttTtttTto..
..ottTtttTtttto.
..otttTttttTtto.
..ottTttTttttto.
...otttttTttto..
...ottTtttttto..
..otTttttTtttto.
..otttoooootTto.
...ooo.....ooo..
................
................
"""
HIDE_COLOURS = {"o": (70, 44, 24), "t": (170, 120, 72), "T": (196, 150, 98)}

CAP = """
................
................
................
.....oooooo.....
....otTttTtto...
...otTttttTtto..
...ottTttttTto..
..otttTtttTttto.
..ottttttttttto.
..oooooooooooooo
.ossssssssssssso
..oooooooooooooo
................
................
................
................
"""
TUNIC = """
................
..ooo......ooo..
.otto.oooo.otto.
.ottTotttTotTto.
.otttTttttTttto.
.ooottTttTtttoo.
...ottttttttto..
...otTttttTtto..
...otttTttttto..
...ossssssssso..
...ottTttTttto..
...otttttTttto..
...ottTttttTto..
...ooooooooooo..
................
................
"""
BOOTS = """
................
................
...oooo..oooo...
...otTo..otTo...
...otto..otto...
...oTto..oTto...
...otto..otto...
...ossoo.ossoo..
..ottttooTtttoo.
.otTtttooottTto.
.otttttoootttto.
.oooooooooooooo.
................
................
................
................
"""
ARMOUR_COLOURS = {"o": (60, 38, 20), "t": (158, 108, 62), "T": (186, 138, 86), "s": (90, 60, 34)}

# The chest: dark-edged wooden boards, a lid seam with an iron latch on the sides.
CHEST_SIDE = """
dddddddddddddddd
dwwwwwWwwwwwwwwd
dwWwwwwwwwwWwwwd
dwwwwwwwwwwwwwwd
dwwwWwwwwwwwwWwd
dwwwwwwwwwwwwwwd
dddddddiiddddddd
dwwwwwwiiwwwwwwd
dwWwwwwkkwwwwwwd
dwwwwwwwwwwWwwwd
dwwwwwWwwwwwwwwd
dwwwwwwwwwwwwwwd
dwwWwwwwwwwwwWwd
dwwwwwwwwwwwwwwd
dwwwwwwwWwwwwwwd
dddddddddddddddd
"""
CHEST_TOP = """
dddddddddddddddd
dwwwwwwwwwwwwwwd
dwWwwwwwwWwwwwwd
dwwwwwwwwwwwwwwd
dllllllllllllld.
dwwwwwwwwwwwwwwd
dwwwWwwwwwwwwWwd
dwwwwwwwwwwwwwwd
dllllllllllllld.
dwwwwwwwwwwwwwwd
dwWwwwwwwwwWwwwd
dwwwwwwwwwwwwwwd
dllllllllllllld.
dwwwwwwWwwwwwwwd
dwwwwwwwwwwwwwwd
dddddddddddddddd
""".replace(".", "d")
CHEST_COLOURS = {"d": (74, 48, 24), "w": (156, 110, 60), "W": (176, 128, 74), "l": (122, 84, 44), "i": (70, 70, 76), "k": (40, 40, 44)}

# The bed, half a block high: a red blanket with a pale pillow at one end on top; the
# sides show the lower half of their texture (the half-height block's side), so the
# frame and blanket edge are drawn there.
BED_TOP = """
pppppppppppppppp
pPPPPPPPPPPPPPPp
pPPPPPPPPPPPPPPp
pPPPPPPPPPPPPPPp
pppppppppppppppp
rrrrrrrrrrrrrrrr
rRrrrrrRrrrrrRrr
rrrrRrrrrrRrrrrr
rrrrrrrrrrrrrrrr
rRrrrrRrrrrrRrrr
rrrrrrrrrrrrrrrr
rrrRrrrrrRrrrrRr
rrrrrrrrrrrrrrrr
rrRrrrrrRrrrrrrr
rrrrrrrrrrrrrrrr
RRRRRRRRRRRRRRRR
"""
BED_SIDE = """
wwwwwwwwwwwwwwww
wwwwwwwwwwwwwwww
wwwwwwwwwwwwwwww
wwwwwwwwwwwwwwww
wwwwwwwwwwwwwwww
wwwwwwwwwwwwwwww
wwwwwwwwwwwwwwww
wwwwwwwwwwwwwwww
rrrrrrrrrrrrrrrr
RrrrRrrrrRrrrrRr
rrrrrrrrrrrrrrrr
dddddddddddddddd
dwwwwwwwwwwwwwwd
dwWwwwwwwwwWwwwd
dwwwwwwWwwwwwwwd
dddddddddddddddd
"""
BED_COLOURS = {"p": (210, 206, 196), "P": (238, 236, 228), "r": (168, 40, 38), "R": (196, 62, 56), "d": (74, 48, 24), "w": (156, 110, 60), "W": (176, 128, 74)}


def main():
    draw("raw_venison", VENISON, MEAT, seed=11)
    draw("cooked_venison", VENISON, COOKED, seed=12)
    draw("tool_sword", SWORD, SWORD_COLOURS, jitter=6, seed=13)
    draw("hide", HIDE, HIDE_COLOURS, seed=14)
    draw("armour_cap", CAP, ARMOUR_COLOURS, seed=15)
    draw("armour_tunic", TUNIC, ARMOUR_COLOURS, seed=16)
    draw("armour_boots", BOOTS, ARMOUR_COLOURS, seed=17)
    draw("chest_side", CHEST_SIDE, CHEST_COLOURS, jitter=6, seed=18)
    draw("chest_top", CHEST_TOP, CHEST_COLOURS, jitter=6, seed=19)
    draw("bed_top", BED_TOP, BED_COLOURS, jitter=6, seed=20)
    draw("bed_side", BED_SIDE, BED_COLOURS, jitter=6, seed=21)


if __name__ == "__main__":
    main()
