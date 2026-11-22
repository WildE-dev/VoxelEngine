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

# Ores: stone with specks of coal or rust-coloured iron.
ORE = """
ssSsssssSssssSss
sssssssssssssssS
ssoossSssssoosss
sooo osssssooosS
ssoo ssssSssooss
sssssssssssssss
sSssssoossssssss
sssssoooosSsssss
ssssssooo ssssss
sssSsssssssssSss
ssssssssssssssss
soossssSsssooss
sooossssssooooss
ssoossssssSoossS
sssssSssssssssss
ssssssssssssssss
""".replace(" ", "s")
ORE = "\n".join(r.ljust(16, "s")[:16] for r in ORE.strip("\n").split("\n"))
COAL_ORE_COLOURS = {"s": (128, 128, 128), "S": (146, 146, 146), "o": (30, 30, 32)}
IRON_ORE_COLOURS = {"s": (128, 128, 128), "S": (146, 146, 146), "o": (196, 140, 104)}

COAL_LUMP = """
................
................
.....kkk........
...kkkKkkk......
..kkKkkkkkk.....
..kkkkkkKkkk....
.kkkKkkkkkkkk...
.kkkkkkkKkkkkk..
.kKkkkkkkkkKkk..
..kkkkKkkkkkk...
..kkkkkkkkKk....
...kkkKkkkk.....
.....kkkk.......
................
................
................
"""
COAL_COLOURS = {"k": (36, 36, 40), "K": (70, 70, 78)}
INGOT = """
................
................
................
................
......oooooooo..
....oiiIiiiiio..
...oiIiiiiiioo..
..oiiiiiiiiooo..
.oooooooooooio..
.oiiiiiiiiioio..
.oiiiIiiiiioo...
.oiiiiiiiiio....
.ooooooooooo....
................
................
................
"""
INGOT_COLOURS = {"o": (90, 90, 96), "i": (196, 196, 204), "I": (236, 236, 242)}

SEEDS = """
................
................
................
.....s....s.....
...s...s......s.
.......S..s.....
..s..s.....s....
....S...s....s..
.s.....s..S.....
...s.s.....s....
......s..s...s..
..S.......s.....
....s..s......s.
................
................
................
"""
SEED_COLOURS = {"s": (170, 150, 80), "S": (120, 100, 50)}
# Crops are drawn like plants: crossed sprites, transparent around the stalks.
CROP = """
................
................
................
................
................
................
................
.......g........
...g...g....g...
...gg..g...gg...
....g.gg..g.....
..g.g..g..g..g..
...gg..gg.g.gg..
....g...g.g.g...
....g...g.g.g...
....g...g.g.g...
"""
CROP_COLOURS = {"g": (86, 168, 52)}
RIPE = """
.....y....y.....
....yYy..yYy..y.
.y..yyy..yyy.yYy
yYy..y....y..yyy
yyy..y.y..y...y.
.y...yyYy.y...y.
.y....yyy.y...y.
.y.y...y..y.y.y.
.yyYy..y..yyYyy.
..yyy..y...yyy..
...y...y....y...
...y...y....y...
...y...y....y...
...y...y....y...
...y...y....y...
...y...y....y...
"""
RIPE_COLOURS = {"y": (200, 170, 70), "Y": (232, 206, 110)}
WHEAT_BUNDLE = """
................
.....y.y.y......
....yYyYyYy.....
....yyyyyyy.....
.....yyyyy......
......yyy.......
......bbb.......
......yyy.......
.....yyyyy......
....yy.y.yy.....
...yy..y..yy....
...y...y...y....
..y....y....y...
................
................
................
"""
WHEAT_COLOURS = {"y": (204, 172, 72), "Y": (234, 208, 112), "b": (130, 90, 40)}
BREAD = """
................
................
................
................
.....oooooo.....
...ooBBbBBboo...
..oBbBBBbBBBbo..
.oBBBbBBBBbBBBo.
.obBBBBbBBBBBbo.
.oBBBBBBBBBBBBo.
.oddddddddddddo.
..oddddddddddo..
...oooooooooo...
................
................
................
"""
BREAD_COLOURS = {"o": (110, 64, 26), "B": (200, 140, 64), "b": (226, 176, 100), "d": (160, 104, 44)}

LADDER = """
.rr..........rr.
.rR..........rR.
.rrwwwwwwwwwwrr.
.rRWWWWWWWWWWrR.
.rr..........rr.
.rR..........rR.
.rr..........rr.
.rRwwwwwwwwwwrR.
.rrWWWWWWWWWWrr.
.rR..........rR.
.rr..........rr.
.rR..........rR.
.rrwwwwwwwwwwrr.
.rRWWWWWWWWWWrR.
.rr..........rr.
.rR..........rR.
"""
LADDER_COLOURS = {"r": (120, 82, 42), "R": (146, 104, 58), "w": (150, 108, 60), "W": (110, 76, 38)}

DOOR = """
dddddddddddddddd
dwwwwwwwwwwwwwwd
dwWwwwwddwwwwWwd
dwwwwwwddwwwwwwd
dwwwWwwddwwwwwwd
dwwwwwwddwwWwwwd
dwwwwwwddwwwwwwd
dwWwwwwddwwwwkkd
dwwwwwwddwwwwkkd
dwwwwWwddwwwwwwd
dwwwwwwddwwwwwwd
dwwwwwwddwWwwwwd
dwWwwwwddwwwwwwd
dwwwwwwddwwwwWwd
dwwwwwwwwwwwwwwd
dddddddddddddddd
"""
DOOR_OPEN = """
dddddddddddddddd
dd............dd
d..............d
d..............d
d..............d
d..............d
d..............d
d..............d
d..............d
d..............d
d..............d
d..............d
d..............d
d..............d
dd............dd
dddddddddddddddd
"""
DOOR_COLOURS = {"d": (90, 58, 28), "w": (160, 114, 64), "W": (180, 132, 78), "k": (60, 60, 66)}

BOW = """
................
.........wwww...
.......ww...s...
......w....s....
.....w....s.....
....w....s......
....w...s.......
...w...s........
...w..s.........
...w.s..........
...ws...........
...s............
..sw............
..w.............
..w.............
................
"""
BOW_COLOURS = {"w": (128, 86, 44), "s": (220, 220, 210)}
ARROW_ICON = """
................
............hhh.
.............hh.
............h.h.
...........w....
..........w.....
.........w......
........w.......
.......w........
......w.........
.....w..........
..ff.w..........
..ffw...........
...ff...........
..f.f...........
................
"""
ARROW_COLOURS = {"h": (150, 150, 156), "w": (140, 98, 52), "f": (232, 232, 226)}

ROD = """
................
.............ww.
............wws.
...........ww.s.
..........ww..s.
.........ww...s.
........ww....s.
.......ww.....s.
......ww......s.
.....ww.......s.
....ww........s.
...ww.........s.
..ww.........rr.
.ww..........rr.
.w..............
................
"""
ROD_COLOURS = {"w": (128, 86, 44), "s": (230, 230, 230), "r": (200, 40, 40)}
FISH = """
................
................
................
................
.........ffff...
......ffffffff..
.ff..ffffeffkff.
.fffffffffffffff
.ffffFFFFFFFFff.
.ff..ffFFFFFff..
......ffffff....
................
................
................
................
................
"""
RAW_FISH_COLOURS = {"f": (120, 150, 170), "F": (210, 220, 225), "e": (20, 20, 20), "k": (90, 110, 130)}
COOKED_FISH_COLOURS = {"f": (170, 110, 60), "F": (220, 170, 110), "e": (40, 30, 20), "k": (130, 80, 40)}
BOBBER = """
................
................
................
................
......rrrr......
.....rrrrrr.....
.....rrrrrr.....
.....wwwwww.....
.....wwwwww.....
......wwww......
................
................
................
................
................
................
"""
BOBBER_COLOURS = {"r": (210, 40, 40), "w": (240, 240, 240)}

CAMPFIRE = """
................
................
.......y........
......yYy.......
.....yYOYy......
....yYOOOYy.....
....yOOrOOy.....
...yOOrrrOOy....
...yOrrRrrOy....
....rrRRRrr.....
...wlwrrrwlw....
..wllwwwwwllw...
.wlllwlllwlllw..
.wwwwwwwwwwwww..
..ss.ss..ss.ss..
................
"""
CAMPFIRE_COLOURS = {"y": (250, 220, 90), "Y": (255, 240, 150), "O": (250, 160, 40), "r": (230, 80, 20), "R": (255, 120, 40),
                    "w": (100, 66, 34), "l": (140, 96, 52), "s": (110, 110, 110)}
SMOKE = """
................
................
.....ssss.......
...ssSSSsss.....
..sSSSSSSSss....
..sSSSSSSSSs....
.ssSSSSSSSSss...
.sSSSSSSSSSSs...
.sSSSSSSSSSss...
..sSSSSSSSSs....
..ssSSSSSSss....
...sssSSsss.....
.....ssss.......
................
................
................
"""
SMOKE_COLOURS = {"s": (120, 120, 124), "S": (160, 160, 164)}

SIGN = """
................
................
.bbbbbbbbbbbbbb.
.bwwwwwwwwwwwwb.
.bwkkkwkkkwkkwb.
.bwwwwwwwwwwwwb.
.bwkkwkkkkwkkwb.
.bwwwwwwwwwwwwb.
.bbbbbbbbbbbbbb.
.......pp.......
.......pp.......
.......pp.......
.......pp.......
.......pp.......
.......pp.......
.......pp.......
"""
SIGN_COLOURS = {"b": (110, 76, 38), "w": (196, 160, 104), "k": (90, 64, 34), "p": (120, 84, 44)}

TNT_SIDE = """
rrRrrrRrrrrRrrRr
rRrrrRrrrRrrrRrr
rrrrrrrrrrrrrrrr
rrRrrrrRrrrRrrrr
wwwwwwwwwwwwwwww
wkkkwkkkwkwwkkkw
wwkwwkwkwkkwwkww
wwkwwkwkwkwkwkww
wwkwwkwkwkwwkkww
wwwwwwwwwwwwwwww
rrrRrrrrrRrrrrRr
rRrrrrRrrrrrRrrr
rrrrRrrrrrRrrrrr
rrRrrrrrRrrrrRrr
rrrrrRrrrrrrrrrr
rRrrrrrRrrrRrrrR
"""
TNT_TOP = """
rrrrrrrrrrrrrrrr
rRrrRrrRrrRrrRrr
rrrrrrrrrrrrrrrr
rrRrrrrkkrrrrRrr
rrrrrrkkkkrrrrrr
rrRrrkkkkkkrrRrr
rrrrrkkggkkrrrrr
rrRrrkkggkkrrRrr
rrrrrkkkkkkrrrrr
rrRrrrkkkkrrrRrr
rrrrrrrkkrrrrrrr
rrRrrrrrrrrrrRrr
rrrrrrrrrrrrrrrr
rRrrRrrRrrRrrRrr
rrrrrrrrrrrrrrrr
rrrrrrrrrrrrrrrr
"""
TNT_COLOURS = {"r": (196, 48, 40), "R": (220, 70, 60), "w": (236, 232, 220), "k": (40, 36, 34), "g": (120, 120, 120)}
TNT_LIT_COLOURS = {"r": (255, 210, 200), "R": (255, 236, 230), "w": (255, 255, 255), "k": (120, 110, 100), "g": (255, 220, 120)}

BOAT_ICON = """
................
................
................
................
................
................
d..............d
dd............dd
dwwwwwwwwwwwwwwd
dwWwwwwWwwwwWwwd
.dwwwwwwwwwwwwd.
..dwwwWwwwwwwd..
...dddddddddd...
................
................
................
"""
BOAT_COLOURS = {"d": (90, 58, 28), "w": (160, 114, 64), "W": (184, 136, 82)}

SAPLING = """
................
................
.......ll.......
.....lLllLl.....
....lLlllllL....
.....llLlll.....
...lLl.ll.lLl...
..lllllllllll...
...lLlLttlLl....
.....ll.t.......
........t.......
........t.......
.......tt.......
........t.......
........t.......
................
"""
SAPLING_COLOURS = {"l": (60, 140, 50), "L": (90, 170, 70), "t": (110, 76, 40)}
PINE_SAPLING_COLOURS = {"l": (40, 96, 50), "L": (60, 120, 66), "t": (90, 60, 34)}


WHEEL_ICON = """
.......dd.......
...d...pp...d...
....d..pp..d....
.....d.dd.d.....
..d...dddd...d..
.pp..dd..dd..pp.
.ppddd.hh.dddpp.
ddddd.hhhh.ddddd
ddddd.hhhh.ddddd
.ppddd.hh.dddpp.
.pp..dd..dd..pp.
..d...dddd...d..
.....d.dd.d.....
....d..pp..d....
...d...pp...d...
.......dd.......
"""
WHEEL_COLOURS = {"d": (92, 62, 34), "p": (160, 114, 62), "h": (110, 110, 116)}

AIR_PIPE = """
oooooooooooooooo
cCCCCCCCCCCCCCCc
cCcccCCCCCCcccCc
ccCCcccccccCCccc
cccccccccccccccc
bccccccccccccccb
cccccccccccccccc
cccccccccccccccc
cccccccccccccccc
cccccccccccccccc
bccccccccccccccb
cccccccccccccccc
cccccccccccccccc
dddddddddddddddd
cCCCCCCCCCCCCCCc
oooooooooooooooo
"""
AIR_PIPE_END = """
oooooooooooooooo
occcccccccccccco
occCCCCCCCCCccco
ocCkkkkkkkkkkCco
ocCkkkkkkkkkkCco
ocCkkkkkkkkkkCco
ocCkkkkkkkkkkCco
ocCkkkkkkkkkkCco
ocCkkkkkkkkkkCco
ocCkkkkkkkkkkCco
ocCkkkkkkkkkkCco
ocCkkkkkkkkkkCco
occCCCCCCCCCCcco
occcccccccccccco
occcbccccccbccco
oooooooooooooooo
"""
AIR_PIPE_COLOURS = {"o": (96, 60, 30), "c": (184, 120, 64), "C": (220, 162, 96), "b": (120, 76, 40), "d": (150, 96, 50), "k": (40, 30, 24)}

DRILL_SIDE = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIrIIIIIIIIIIrIi
iIIIIIIIIIIIIIIi
iIIggggggggggIIi
iIIgyyyyyyyygIIi
iIIgyyyyyyyygIIi
iIIggggggggggIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
gssssssssssssssg
ggssSSSSSSSSssgg
gggsssSSSSsssggg
ggggssSSSSssgggg
ggggggssssgggggg
"""
DRILL_TOP = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIrIIIIIIIIIIrIi
iIIIIkkkkkkIIIIi
iIIIkIIIIIIkIIIi
iIIkIkkkkkkIkIIi
iIIkIkIIIIkIkIIi
iIIkIkIccIkIkIIi
iIIkIkIccIkIkIIi
iIIkIkIIIIkIkIIi
iIIkIkkkkkkIkIIi
iIIIkIIIIIIkIIIi
iIIIIkkkkkkIIIIi
iIrIIIIIIIIIIrIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
DRILL_BOTTOM = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIIssssssssssIIi
iIssSSSSSSSSssIi
iIsSSssssssSSsIi
iIsSsSSSSSSsSsIi
iIsSsSsssSSsSsIi
iIsSsSskkSsSsSIi
iIsSsSskkSsSsSIi
iIsSsSSssSSsSsIi
iIsSsSSSSSSsSsIi
iIsSSssssssSSsIi
iIssSSSSSSSSssIi
iIIssssssssssIIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
DRILL_COLOURS = {"i": (60, 60, 66), "I": (118, 118, 126), "r": (180, 180, 186), "g": (40, 40, 44), "y": (210, 170, 60),
                 "k": (36, 36, 40), "c": (184, 120, 64), "s": (150, 150, 158), "S": (196, 196, 204)}

SHAFT_ICON = """
..............ii
.............iwi
............iwwi
...........iwwi.
..........iwwi..
.........iwwi...
........iiii....
.......iwwi.....
......iwwi......
.....iiii.......
....iwwi........
...iwwi.........
..iwwi..........
.iwwi...........
iwwi............
iii.............
"""
SHAFT_COLOURS = {"i": (90, 90, 96), "w": (150, 106, 58)}

COMPRESSOR_SIDE = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIrIIIIIIIIIIrIi
iIIIIwwwwwwIIIIi
iIIIwWWWWWWwIIIi
iIIwWWWWWWkWwIIi
iIIwWWWWWkWWwIIi
iIIwWWWWkWWWwIIi
iIIwWWWkWWWWwIIi
iIIwWWWWWWWWwIIi
iIIIwWWWWWWwIIIi
iIIIIwwwwwwIIIIi
iIIIIIIIIIIIIIIi
iIrIIIIIIIIIIrIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
COMPRESSOR_TOP = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIgggIgggIgggIIi
iIgggIgggIgggIIi
iIIIIIIIIIIIIIIi
iIgggIgggIgggIIi
iIgggIgggIgggIIi
iIIIIIIIIIIIIIIi
iIgggIgggIgggIIi
iIgggIgggIgggIIi
iIIIIIIIIIIIIIIi
iIgggIgggIgggIIi
iIgggIgggIgggIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
COMPRESSOR_COLOURS = {"i": (70, 52, 36), "I": (176, 116, 62), "r": (220, 170, 110), "w": (60, 60, 64), "W": (230, 228, 214),
                      "k": (200, 40, 36), "g": (50, 36, 26)}

LEVER = """
................
................
................
.........hh.....
........hhh.....
........hh......
.......ww.......
.......ww.......
......ww........
......ww........
.....ww.........
....sssssss.....
...sSSSSSSSs....
...sSSSSSSSs....
...sssssssss....
................
"""
LEVER_ON = """
................
................
................
....hh..........
....hhh.........
.....hh.........
.......ww.......
.......ww.......
........ww......
........ww......
.........ww.....
....sssssss.....
...sSSSSSSSs....
...sSSSSSSSs....
...sssssssss....
................
"""
LEVER_COLOURS = {"w": (120, 84, 46), "h": (90, 90, 96), "s": (100, 100, 104), "S": (140, 140, 146)}
LEVER_ON_COLOURS = {"w": (120, 84, 46), "h": (220, 60, 50), "s": (100, 100, 104), "S": (140, 140, 146)}

GATE_TOP = """
ssssssssssssssss
sSSSSSSSSSSSSSSs
sSccccSSSSccccSs
sScoocSSSScoocSs
sSccccSSSSccccSs
sSSSSyyyyyySSSSs
sSSSSySSSSySSSSs
sSSSSySggSySSSSs
sSSSSySggSySSSSs
sSSSSySSSSySSSSs
sSSSSyyyyyySSSSs
sSSSSSSyySSSSSSs
sSSSSSccccSSSSSs
sSSSSScoocSSSSSs
sSSSSSccccSSSSSs
ssssssssssssssss
"""
GATE_SIDE = """
ssssssssssssssss
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSkkkkkkkkkkkkSs
sSkyykggkyykkkSs
sSkkkkkkkkkkkkSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
ssssssssssssssss
"""
GATE_COLOURS = {"s": (70, 70, 76), "S": (124, 124, 130), "c": (184, 120, 64), "o": (40, 30, 24), "y": (210, 170, 60), "g": (80, 200, 90), "k": (36, 36, 40)}

SIGNAL_LAMP = """
iiiiiiiiiiiiiiii
igggggggggggggGi
igGGGGGGGGGGGGgi
igGggggggggggGgi
igGgGGGGGGGGgGgi
igGgGggggggGgGgi
igGgGgGGGGgGgGgi
igGgGgGrrGgGgGgi
igGgGgGrrGgGgGgi
igGgGgGGGGgGgGgi
igGgGggggggGgGgi
igGgGGGGGGGGgGgi
igGggggggggggGgi
igGGGGGGGGGGGGgi
iggggggggggggggi
iiiiiiiiiiiiiiii
"""
SIGNAL_LAMP_COLOURS = {"i": (60, 60, 66), "g": (110, 40, 36), "G": (140, 56, 50), "r": (170, 70, 60)}
SIGNAL_LAMP_ON_COLOURS = {"i": (60, 60, 66), "g": (240, 90, 70), "G": (255, 150, 110), "r": (255, 240, 200)}

TANK_SIDE = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
irIIIIIIIIIIIIri
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIIIIwwwwwwIIIIi
iIIIwWWWWWWwIIIi
iIIIwWWWkWWwIIIi
iIIIwWWkWWWwIIIi
iIIIIwwwwwwIIIIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
irIIIIIIIIIIIIri
iiiiiiiiiiiiiiii
"""
TANK_TOP = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIIiiiiiiiIIIIi
iIIiIIIIIIIiIIIi
iIiIIIIIIIIIiIIi
iIiIIIIccIIIiIIi
iIiIIIIccIIIiIIi
iIiIIIIIIIIIiIIi
iIIiIIIIIIIiIIIi
iIIIiiiiiiiIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
TANK_COLOURS = {"i": (60, 64, 70), "I": (150, 156, 164), "r": (200, 204, 210), "w": (60, 60, 64), "W": (230, 228, 214), "k": (200, 40, 36), "c": (184, 120, 64)}

VALVE = """
cccccccccccccccc
cCCCCCCCCCCCCCCc
cCCCCCrrrrCCCCCc
cCCCCrCCCCrCCCCc
cCCCrCCCCCCrCCCc
cCCrCCCrrCCCrCCc
cCCrCCrrrrCCrCCc
cCCrCCrrrrCCrCCc
cCCrCCCrrCCCrCCc
cCCCrCCCCCCrCCCc
cCCCCrCCCCrCCCCc
cCCCCCrrrrCCCCCc
cCCCCCCCCCCCCCCc
cCCCCCCCCCCCCCCc
cCCCCCCCCCCCCCCc
cccccccccccccccc
"""
VALVE_COLOURS = {"c": (96, 60, 30), "C": (184, 120, 64), "r": (200, 40, 36)}
VALVE_OPEN_COLOURS = {"c": (96, 60, 30), "C": (184, 120, 64), "r": (60, 180, 70)}


PISTON_FACE = """
iiiiiiiiiiiiiiii
iwwwwwwwwwwwwwwi
iwWWWWWWWWWWWWwi
iwWwwwwwwwwwwWwi
iwWWWWWWWWWWWWwi
iwwwwwwwwwwwwwwi
iwWWWWWWWWWWWWwi
iwWwwwwwwwwwwWwi
iwWWWWWWWWWWWWwi
iwwwwwwwwwwwwwwi
iwWWWWWWWWWWWWwi
iwWwwwwwwwwwwWwi
iwWWWWWWWWWWWWwi
iwwwwwwwwwwwwwwi
iwwwwwwwwwwwwwwi
iiiiiiiiiiiiiiii
"""
PISTON_SIDE = """
wwwwwwwwwwwwwwww
WWWWWWWWWWWWWWWW
wwwwwwwwwwwwwwww
iiiiiiiiiiiiiiii
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSkkSSSSSSs
sSSSSSSkkSSSSSSs
sSSSSSSkkSSSSSSs
sSSSSSSkkSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
ssssssssssssssss
"""
PISTON_BACK = """
ssssssssssssssss
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSiiiiiiiiSSSs
sSSiIIIIIIIIiSSs
sSSiIIIIIIIIiSSs
sSSiIIIkkIIIiSSs
sSSiIIkkkkIIiSSs
sSSiIIkkkkIIiSSs
sSSiIIIkkIIIiSSs
sSSiIIIIIIIIiSSs
sSSiIIIIIIIIiSSs
sSSSiiiiiiiiSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
ssssssssssssssss
"""
PISTON_ROD = """
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIrIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIrIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIIIIrIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIrIi
iIIIIIIIIIIIIIIi
"""
PISTON_COLOURS = {"i": (72, 74, 80), "I": (150, 152, 160), "r": (120, 100, 90), "w": (150, 112, 66), "W": (178, 136, 84),
                  "s": (70, 70, 76), "S": (124, 124, 130), "k": (36, 36, 40)}

SENSOR_FRAME = """
ssssssssssssssss
sSSSSSSSSSSSSSSs
sSkkkkkkkkkkkkSs
sSk..........kSs
sSk..........kSs
sSk..........kSs
sSk..........kSs
sSk..........kSs
sSk..........kSs
sSk..........kSs
sSk..........kSs
sSk..........kSs
sSk..........kSs
sSkkkkkkkkkkkkSs
sSSSSSSSSSSSSSSs
ssssssssssssssss
"""
def sensor(inner):
    rows = SENSOR_FRAME.strip("\n").split("\n")
    inner = inner.strip("\n").split("\n")
    out = []
    for y, r in enumerate(rows):
        if 3 <= y <= 12:
            r = r[:3] + inner[y - 3] + r[13:]
        out.append(r)
    return "\n" + "\n".join(out) + "\n"
WATER_SENSOR = sensor("""
bbbbbbbbbb
bbbbbbbbbb
bbbbBbbbbb
bbbBBBbbbb
bbBBBBBbbb
bbBBwBBbbb
bbBBBBBbbb
bbbBBBbbbb
bbbbbbbbbb
bbbbbbbbbb
""")
PRESSURE_SENSOR = sensor("""
gggggggggg
ggGGGGGGgg
gGGggggGGg
gGgggggrGg
gGggggrgGg
gGgggkggGg
gGggggggGg
ggGGGGGGgg
gggggggggg
gggggggggg
""")
DAYLIGHT_SENSOR = sensor("""
dddddddddd
ddddyddddd
dyddyddyd d
ddyyyyyddd
yyyYYYyyyd
ddyyyyyddd
dyddyddydd
ddddyddddd
dddddddddd
dddddddddd
""".replace(" ", ""))
PLAYER_SENSOR = sensor("""
pppppppppp
ppppPPpppp
pppPeePppp
pppPPPPppp
ppppPPpppp
pppPPPPppp
ppPPPPPPpp
ppPPPPPPpp
pppppppppp
pppppppppp
""")
SENSOR_SIDE = """
ssssssssssssssss
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSkkkkkkkkkkSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
sSSkkkkkkkkkkSSs
sSSSSSSSSSSSSSSs
sSSSSSSSSSSSSSSs
ssssssssssssssss
"""
SENSOR_COLOURS = {"s": (70, 70, 76), "S": (124, 124, 130), "k": (36, 36, 40), "b": (40, 70, 140), "B": (90, 150, 230), "w": (220, 240, 255),
                  "g": (30, 34, 30), "G": (180, 180, 170), "r": (210, 60, 50), "d": (40, 60, 110), "y": (230, 190, 60), "Y": (255, 240, 160),
                  "p": (50, 40, 50), "P": (200, 120, 200), "e": (255, 255, 255)}

ITEM_PIPE = """
oooooooooooooooo
wWWWWWWWWWWWWWWw
wWWWWWWWWWWWWWWw
iiiiiiiiiiiiiiii
wwwwwwwwwwwwwwww
wWWWWWWWWWWWWWWw
wWWWWWWWWWWWWWWw
wwwwwwwwwwwwwwww
wWWWWWWWWWWWWWWw
wWWWWWWWWWWWWWWw
wwwwwwwwwwwwwwww
wWWWWWWWWWWWWWWw
iiiiiiiiiiiiiiii
wWWWWWWWWWWWWWWw
wWWWWWWWWWWWWWWw
oooooooooooooooo
"""
ITEM_PIPE_END = """
iiiiiiiiiiiiiiii
iwwwwwwwwwwwwwwi
iwWWWWWWWWWWWWwi
iwWooooooooooWwi
iwWoooooooooOWwi
iwWoooooooooOWwi
iwWoooooooooOWwi
iwWoooooooooOWwi
iwWoooooooooOWwi
iwWoooooooooOWwi
iwWoooooooooOWwi
iwWoOOOOOOOOOWwi
iwWWWWWWWWWWWWwi
iwwwwwwwwwwwwwwi
iwwwwwwwwwwwwwwi
iiiiiiiiiiiiiiii
"""
ITEM_PIPE_COLOURS = {"o": (40, 30, 22), "O": (70, 52, 36), "w": (132, 96, 56), "W": (164, 124, 76), "i": (110, 112, 118)}
SMELTER_SIDE = """
bbbbbbbbbbbbbbbb
bBBBbBBBBbBBBBbb
bbbbbbbbbbbbbbbb
BBbBBBBbBBBBbBBB
bbbbbbbbbbbbbbbb
bBBBbBBBBbBBBBbb
bbbbbbbbbbbbbbbb
BBbBBBBbBBBBbBBB
bbbbbbbbbbbbbbbb
bBBBbBBBBbBBBBbb
bbbbbbbbbbbbbbbb
BBbBBBBbBBBBbBBB
bbbbbbbbbbbbbbbb
bBBBbBBBBbBBBBbb
bbbbbbbbbbbbbbbb
iiiiiiiiiiiiiiii
"""
SMELTER_FRONT = """
bbbbbbbbbbbbbbbb
bBBBbBBBBbBBBBbb
bbbbbbbbbbbbbbbb
BBbBiiiiiiiibBBB
bbbbikkkkkkibbbb
bBBBikkkkkkiBBbb
bbbbikkkkkkibbbb
BBbBikkkkkkibBBB
bbbbikfffkkibbbb
bBBBikFfFfkiBBbb
bbbbiiiiiiiibbbb
BBbBBBBbBBBBbBBB
bbbbbbbbbbbbbbbb
bBBBbBBBBbBBBBbb
bbbbbbbbbbbbbbbb
iiiiiiiiiiiiiiii
"""
SMELTER_TOP = """
iiiiiiiiiiiiiiii
ibbbbbbbbbbbbbbi
ibBBBBBBBBBBBBbi
ibBkkkkkkkkkkBbi
ibBkKKKKKKKKkBbi
ibBkKkkkkkkKkBbi
ibBkKkkkkkkKkBbi
ibBkKkkkkkkKkBbi
ibBkKkkkkkkKkBbi
ibBkKkkkkkkKkBbi
ibBkKkkkkkkKkBbi
ibBkKKKKKKKKkBbi
ibBkkkkkkkkkkBbi
ibBBBBBBBBBBBBbi
ibbbbbbbbbbbbbbi
iiiiiiiiiiiiiiii
"""
SMELTER_COLOURS = {"b": (96, 92, 90), "B": (130, 124, 120), "i": (70, 72, 78), "k": (24, 22, 22), "K": (50, 46, 44),
                   "f": (60, 40, 30), "F": (90, 60, 40)}
SMELTER_LIT_COLOURS = dict(SMELTER_COLOURS, k=(200, 80, 20), f=(255, 200, 60), F=(255, 140, 30))

SPRING = """
sSsssSSsssSsssSs
sswwSSsssWwssSss
sSwbbwssswbwsssS
ssbBBbwsSwbbwSss
sSwbBbwsswbBbwss
ssswbwssSswbwsSs
sSsswsssssswssss
SsssSSssWwsssSss
sswwsssWbbwssssS
sSwbbwsWbBbwsSss
sswbBbwswbbwssSs
sSswbbwSsswsssss
SssswwssSssswwSs
sSssssSsssswbbws
ssSsssssSsswbBws
sSssSssssSsswwss
"""
SPRING_COLOURS = {"s": (96, 104, 100), "S": (118, 128, 122), "w": (70, 96, 104), "b": (60, 110, 170), "B": (120, 170, 230), "W": (80, 120, 120)}


EXTRACTOR_INTAKE = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIkkkkkkkkkkkkIi
iIkKKKKKKKKKKkIi
iIkKkkkkkkkkKkIi
iIkKkKKKKKKkKkIi
iIkKkKkkkkKkKkIi
iIkKkKkyykKkKkIi
iIkKkKkyykKkKkIi
iIkKkKkkkkKkKkIi
iIkKkKKKKKKkKkIi
iIkKkkkkkkkkKkIi
iIkKKKKKKKKKKkIi
iIkkkkkkkkkkkkIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
EXTRACTOR_OUT = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIIIwwwwwwIIIIi
iIIIwWWWWWWwIIIi
iIIwWWWWWWWWwIIi
iIIwWWkkkkWWwIIi
iIIwWWkkkkWWwIIi
iIIwWWkkkkWWwIIi
iIIwWWkkkkWWwIIi
iIIwWWWWWWWWwIIi
iIIIwWWWWWWwIIIi
iIIIIwwwwwwIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
EXTRACTOR_SIDE = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIIIIIyIIIIIIIIi
iIIIIIyyIIIIIIIi
iIyyyyyyyIIIIIIi
iIyyyyyyyyIIIIIi
iIyyyyyyyIIIIIIi
iIIIIIyyIIIIIIIi
iIIIIIyIIIIIIIIi
iIIIIIIIIIIIIIIi
iwwwwwwwwwwwwwwi
iWWWWWWWWWWWWWWi
iwwwwwwwwwwwwwwi
iWWWWWWWWWWWWWWi
iwwwwwwwwwwwwwwi
iiiiiiiiiiiiiiii
"""
EXTRACTOR_COLOURS = {"i": (72, 74, 80), "I": (140, 142, 150), "k": (30, 30, 34), "K": (60, 60, 66), "y": (210, 170, 60),
                     "w": (132, 96, 56), "W": (164, 124, 76)}
FILTER_PIPE_COLOURS = {"o": (40, 30, 22), "O": (70, 52, 36), "w": (60, 110, 120), "W": (90, 150, 160), "i": (110, 112, 118)}
CRAFTER_TOP = """
wwwwwwwwwwwwwwww
wWWWWWWWWWWWWWWw
wWiiiiWiiiiWiiWw
wWiIIiWiIIiWiIWw
wWiIIiWiIIiWiIWw
wWiiiiWiiiiWiiWw
wWWWWWWWWWWWWWWw
wWiiiiWiiiiWiiWw
wWiIIiWiIIiWiIWw
wWiIIiWiIIiWiIWw
wWiiiiWiiiiWiiWw
wWWWWWWWWWWWWWWw
wWiiiiWiiiiWiiWw
wWiIIiWiIIiWiIWw
wWWWWWWWWWWWWWWw
wwwwwwwwwwwwwwww
"""
CRAFTER_SIDE = """
wwwwwwwwwwwwwwww
wWWWWWWWWWWWWWWw
wWssssssssssssWw
wWsggggggggggsWw
wWsgSSgggSSggsWw
wWsgSSgggSSggsWw
wWsggggggggggsWw
wWsggggSSggggsWw
wWsggggSSggggsWw
wWsggggggggggsWw
wWssssssssssssWw
wWWWWWWWWWWWWWWw
wwwwwwwwwwwwwwww
wWWWWWWWWWWWWWWw
wWWWWWWWWWWWWWWw
wwwwwwwwwwwwwwww
"""
CRAFTER_BOTTOM = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIwwwwwwwwwwIIi
iIIwWWWWWWWWwIIi
iIIwWWWWWWWWwIIi
iIIwWWWWWWWWwIIi
iIIwWWWWWWWWwIIi
iIIwWWWWWWWWwIIi
iIIwWWWWWWWWwIIi
iIIwWWWWWWWWwIIi
iIIwWWWWWWWWwIIi
iIIwwwwwwwwwwIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
CRAFTER_COLOURS = {"w": (132, 96, 56), "W": (164, 124, 76), "i": (72, 74, 80), "I": (140, 142, 150), "s": (60, 60, 66),
                   "g": (180, 182, 190), "S": (90, 120, 200)}
HARVESTER_TOP = """
wwwwwwwwwwwwwwww
wWWWWWWWWWWWWWWw
wWWWWWiiiiWWWWWw
wWWWWiIIIIiWWWWw
wWWWiIyyyyIiWWWw
wWWiIyiyyiyIiWWw
wWiIyyyyyyyyIiWw
wWiIyyyiiyyyIiWw
wWiIyyyiiyyyIiWw
wWiIyyyyyyyyIiWw
wWWiIyiyyiyIiWWw
wWWWiIyyyyIiWWWw
wWWWWiIIIIiWWWWw
wWWWWWiiiiWWWWWw
wWWWWWWWWWWWWWWw
wwwwwwwwwwwwwwww
"""
HARVESTER_SIDE = """
wwwwwwwwwwwwwwww
wWWWWWWWWWWWWWWw
wWWWWWWWWWWWWWWw
iiiiiiiiiiiiiiii
IyIyIyIyIyIyIyIy
yIyIyIyIyIyIyIyI
IyIyIyIyIyIyIyIy
iiiiiiiiiiiiiiii
wWWWWWWWWWWWWWWw
wWWWWWWWWWWWWWWw
wWWWWWWWWWWWWWWw
wWWWWWWWWWWWWWWw
wWWWWWWWWWWWWWWw
wWWWWWWWWWWWWWWw
wWWWWWWWWWWWWWWw
wwwwwwwwwwwwwwww
"""
HARVESTER_COLOURS = {"w": (132, 96, 56), "W": (164, 124, 76), "i": (72, 74, 80), "I": (170, 172, 180), "y": (214, 180, 70)}

DRAIN_TOP = """
iiiiiiiiiiiiiiii
iIkIkIkIkIkIkIIi
iIkIkIkIkIkIkIIi
iIkIkIkIkIkIkIIi
iIkIkIkIkIkIkIIi
iIkIkIkIkIkIkIIi
iIkIkIkIkIkIkIIi
iIkIkIkIkIkIkIIi
iIkIkIkIkIkIkIIi
iIkIkIkIkIkIkIIi
iIkIkIkIkIkIkIIi
iIkIkIkIkIkIkIIi
iIkIkIkIkIkIkIIi
iIkIkIkIkIkIkIIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
DRAIN_SIDE = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
sSsssSSsssSsssSs
ssssSSssssssssss
sSsssssSssssSsss
ssSsssssssSsssss
sssssSsssssssSss
sSsssssssSssssss
ssssSssssssssSss
sSssssssSsssssss
ssssssSsssssSsss
sSsssssssssSssss
ssssSsssssssssSs
ssSsssSssssssSss
sSssssssSsssssss
"""
DRAIN_COLOURS = {"i": (60, 62, 68), "I": (120, 124, 132), "k": (14, 16, 22), "s": (96, 104, 100), "S": (118, 128, 122)}

SINKHOLE_TOP = """
sSsssSSsssSsssSs
ssssSSsssssssSss
sSsssdddddssssss
sssdddkkkddddSss
ssddkkkkkkkddsss
sSdkkkkkkkkkdsSs
ssdkkkKkkkkkdsss
sddkkkkkkkkkddss
sdkkkkkkkKkkkdss
sdkkkKkkkkkkkdSs
ssdkkkkkkkkkdsss
sSddkkkkkkkddsss
sssdddkkkdddssSs
ssSssddddddsssss
sSsssssSsssssSss
ssSssSsssssSssss
"""
SINKHOLE_SIDE = """
sSsssSSsssSsssSs
ssssSSssssssssss
sSsssssSssssSsss
ssSssddsssSsssss
sssssSdssssssSss
sSsssssssSssssss
ssssSsssssssdSss
sSssssssSsssdsss
ssssssSsssssSsss
sSsddssssssSssss
ssssdsssSsssssSs
ssSsssSssssssSss
sSssssssSsssssss
ssssSssssssSssss
sSsssssSssssssSs
ssSssSsssssSssss
"""
SINKHOLE_COLOURS = {"s": (78, 86, 84), "S": (98, 108, 104), "d": (50, 58, 60), "k": (14, 18, 22), "K": (40, 60, 80)}

GAUGE_ICON = """
................
.....kkkkkk.....
...kkwwwwwwkk...
..kwwkwwwwkwwk..
..kwwwwwwwwwwk..
.kwkwwwwwwwwkwk.
.kwwwwwwwwwrwwk.
.kwwwwwwwwrwwwk.
.kwwwwwkkrwwwwk.
.kwgwwwkkwwwkwk.
..kwgwwwwwwwwk..
..kwwgwwwwwwwk..
...kkwwwwwwkk...
.....kkkkkk.....
.......ii.......
......iiii......
"""
GAUGE_ICON_COLOURS = {"k": (60, 60, 66), "w": (236, 230, 205), "r": (200, 30, 25), "g": (60, 160, 70), "i": (110, 112, 118)}

OIL_SHALE = """
sSsssSSssskksSss
sskksSssssskksss
sSkkksSsSssssSss
ssskssssskkssSss
sSsssSsskkkkssss
ssSsssssskksSsss
skksSssSssssssss
skkkssssssSsskks
ssksSssskkssskkk
sSsssSsskkksssks
sssSssssskssSsss
sSssskksssssSsss
ssSskkkssSssskks
sSssskssssssskkk
ssSssssSskksssks
sSssSssssskksSss
"""
OIL_SHALE_COLOURS = {"s": (70, 66, 62), "S": (88, 84, 78), "k": (20, 18, 18)}
TAR = """
kkkKkkkkkKkkkkkk
kKkkkkkkkkkkKkkk
kkkkkkKkkkkkkkkk
kkkkkkkkkkkkkkKk
kkKkkkkkkKkkkkkk
kkkkkkkkkkkkkkkk
kkkkkKkkkkkkKkkk
kKkkkkkkkkkkkkkk
kkkkkkkkKkkkkkkk
kkkkkkkkkkkkkkkk
kkkKkkkkkkkKkkkk
kkkkkkkkkkkkkkkk
kkkkkkKkkkkkkkKk
kKkkkkkkkkkkkkkk
kkkkkkkkkkKkkkkk
kkkkKkkkkkkkkkkk
"""
TAR_SIDE = """
kkkKkkkkkKkkkkkk
kKkkkkkkkkkkKkkk
kkkkkkKkkkkkkkkk
dddkkdddddkkdddd
dDddddddDdddddDd
ddddDdddddddDddd
dddddddDdddddddd
dDddddddddddddDd
ddddddDddddDdddd
ddDddddddddddddd
ddddddddDddddDdd
dDdddDdddddddddd
dddddddddddDdddd
ddddDdddDddddddd
dDdddddddddddddd
ddddddDddddDdddd
"""
TAR_COLOURS = {"k": (22, 20, 22), "K": (60, 58, 70), "d": (110, 80, 52), "D": (130, 96, 64)}
FLUID_PIPE_COLOURS = {"o": (20, 30, 40), "O": (40, 60, 70), "w": (90, 96, 104), "W": (130, 136, 146), "i": (60, 62, 68)}
FLUID_TANK_SIDE = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIggggggggggggIi
iIgGGGGGGGGGGgIi
iIgGGGGGGGGGGgIi
iIgGGGGGGGGGGgIi
iIgGGGGGGGGGGgIi
iIgbbbbbbbbbbgIi
iIgbBbbbbbBbbgIi
iIgbbbbBbbbbbgIi
iIgbbbbbbbbBbgIi
iIgbBbbbbbbbbgIi
iIggggggggggggIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
FLUID_TANK_TOP = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIIIIiiiiiiIIIIi
iIIIiIIIIIIiIIIi
iIIiIIIIIIIIiIIi
iIiIIIIIIIIIIiIi
iIiIIIIkkIIIIiIi
iIiIIIIkkIIIIiIi
iIiIIIIIIIIIIiIi
iIiIIIIIIIIIIiIi
iIIiIIIIIIIIiIIi
iIIIiIIIIIIiIIIi
iIIIIiiiiiiIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
FLUID_TANK_COLOURS = {"i": (60, 62, 68), "I": (120, 124, 132), "g": (90, 110, 120), "G": (170, 200, 210), "b": (40, 40, 44), "B": (70, 70, 80), "k": (20, 20, 22)}
DERRICK_SIDE = """
kkkkkkkkkkkkkkkk
kyyyyyyyyyyyyyyk
kykkkkkkkkkkkkyk
kykyyyyyyyyyykyk
kykykkkkkkkkykyk
kykykyyyyyykykyk
kykykykkkkykykyk
kykykyk..kykykyk
kykykyk..kykykyk
kykykykkkkykykyk
kykykyyyyyykykyk
kykykkkkkkkkykyk
kykyyyyyyyyyykyk
kykkkkkkkkkkkkyk
kyyyyyyyyyyyyyyk
kkkkkkkkkkkkkkkk
"""
DERRICK_TOP = """
kkkkkkkkkkkkkkkk
kIIIIIIIIIIIIIIk
kIiiiiiiiiiiiiIk
kIiIIIIIIIIIIiIk
kIiIyyyyyyyyIiIk
kIiIykkkkkkyIiIk
kIiIykIIIIkyIiIk
kIiIykIkkIkyIiIk
kIiIykIkkIkyIiIk
kIiIykIIIIkyIiIk
kIiIykkkkkkyIiIk
kIiIyyyyyyyyIiIk
kIiIIIIIIIIIIiIk
kIiiiiiiiiiiiiIk
kIIIIIIIIIIIIIIk
kkkkkkkkkkkkkkkk
"""
DERRICK_COLOURS = {"k": (30, 30, 34), "y": (200, 160, 40), "I": (130, 132, 140), "i": (70, 72, 78), ".": (0, 0, 0)}
REFINERY_SIDE = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIrrrIIIIIIrrrIi
iIrRrIIIIIIrRrIi
iIrRrIIIIIIrRrIi
iIrRrIIkkIIrRrIi
iIrRrIkookIrRrIi
iIrRrIkookIrRrIi
iIrRrIIkkIIrRrIi
iIrRrIIIIIIrRrIi
iIrRrIIIIIIrRrIi
iIrrrIIIIIIrrrIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
REFINERY_IN = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIIIIIkkkkIIIIIi
iIIIIkddddkIIIIi
iIIIkddddddkIIIi
iIIkddddddddkIIi
iIIkddddddddkIIi
iIIkddddddddkIIi
iIIkddddddddkIIi
iIIIkddddddkIIIi
iIIIIkddddkIIIIi
iIIIIIkkkkIIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
REFINERY_COLOURS = {"i": (60, 62, 68), "I": (140, 142, 150), "r": (150, 60, 40), "R": (200, 90, 50), "k": (24, 24, 28), "o": (230, 170, 50), "d": (40, 30, 26)}
REFINERY_OUT_COLOURS = dict(REFINERY_COLOURS, d=(220, 180, 60))
ENGINE_SIDE = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIkkkkkkkkkkkkIi
iIkrrrrrrrrrrkIi
iIkrRRRRRRRRrkIi
iIkrRkkkkkkRrkIi
iIkrRkIIIIkRrkIi
iIkrRkIkkIkRrkIi
iIkrRkIkkIkRrkIi
iIkrRkIIIIkRrkIi
iIkrRkkkkkkRrkIi
iIkrRRRRRRRRrkIi
iIkrrrrrrrrrrkIi
iIkkkkkkkkkkkkIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
ENGINE_TOP = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIkIkIkIkIkIkIIi
iIkIkIkIkIkIkIIi
iIIIIIIIIIIIIIIi
iIkIkIkIkIkIkIIi
iIkIkIkIkIkIkIIi
iIIIIIIIIIIIIIIi
iIIIIIkkkkIIIIIi
iIIIIkkookkIIIIi
iIIIIkkookkIIIIi
iIIIIIkkkkIIIIIi
iIIIIIIIIIIIIIIi
iIkIkIkIkIkIkIIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
ENGINE_COLOURS = {"i": (50, 50, 56), "I": (110, 112, 120), "k": (24, 24, 28), "r": (120, 40, 30), "R": (170, 60, 40), "o": (230, 140, 40)}
OUTLET_FACE = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIIIwwwwwwIIIIi
iIIIwbbbbbbwIIIi
iIIwbbBbbbbbwIIi
iIIwbbbbbBbbwIIi
iIIwbbbbbbbbwIIi
iIIwbBbbbbbbwIIi
iIIwbbbbbbBbwIIi
iIIIwbbbbbbwIIIi
iIIIIwwwwwwIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
OUTLET_COLOURS = {"i": (60, 62, 68), "I": (120, 124, 132), "w": (80, 84, 92), "b": (40, 70, 140), "B": (90, 150, 230)}

CRUSHER_SIDE = """
kkkkkkkkkkkkkkkk
kmmmmmmmmmmmmmmk
kmMMMMMMMMMMMMmk
kmMkkkkkkkkkkMmk
kmMkyyyyyyyykMmk
kmMkkkkkkkkkkMmk
kmMMMMMMMMMMMMmk
kmmmmmmmmmmmmmmk
kmiiiiiiiiiiiimk
kmiIiIiIiIiIiimk
kmiiiiiiiiiiiimk
kmiIiIiIiIiIiimk
kmiiiiiiiiiiiimk
kmmmmmmmmmmmmmmk
kmmrmmmmmmmmrmmk
kkkkkkkkkkkkkkkk
"""
CRUSHER_TOP = """
kkkkkkkkkkkkkkkk
kmmmmmmmmmmmmmmk
kmkkkkkkkkkkkkmk
kmkIiIiIiIiIikmk
kmkiIiIiIiIiIkmk
kmkIiIiIiIiIikmk
kmkkkkkkkkkkkkmk
kmkkkkkkkkkkkkmk
kmkIiIiIiIiIikmk
kmkiIiIiIiIiIkmk
kmkIiIiIiIiIikmk
kmkkkkkkkkkkkkmk
kmmmmmmmmmmmmmmk
kmMMMMMMMMMMMMmk
kmmmmmmmmmmmmmmk
kkkkkkkkkkkkkkkk
"""
CRUSHER_COLOURS = {"k": (34, 36, 40), "m": (96, 100, 108), "M": (128, 132, 140), "i": (70, 72, 78), "I": (160, 164, 172), "y": (210, 170, 40), "r": (150, 50, 40)}
CRUSHED_IRON = """
................
................
................
.......oo.......
.....ooIoo......
....oIiioIo.....
...oiIoiIioo....
..oIiooIiioIo...
.oiiIoiioIiiIo..
.oIoiiIoiIoiio..
oiiIoiIiiIoIiIo.
oIoiiIoiIoiiIio.
.oooooooooooooo.
................
................
................
"""
CRUSHED_IRON_COLOURS = {"o": (60, 52, 48), "i": (150, 128, 112), "I": (196, 180, 168)}
COKE = """
................
................
.....ooooo......
...ookkKkkoo....
..okkKkkkKkko...
..okKkgkkkKko...
.okkkkkKkgkkko..
.okKkgkkkkkKko..
.okkkkKkkgkkko..
.okgkkkkKkkkko..
..okkKkkgkkKko..
..okkkkKkkkko...
...ookkkkkoo....
.....ooooo......
................
................
"""
COKE_COLOURS = {"o": (20, 20, 22), "k": (52, 52, 56), "K": (84, 84, 90), "g": (130, 126, 112)}

GLOW_FUNGUS = """
................
................
.....cccc.......
....cCCCCc......
...cCCwCCCc.....
...cccccccc..cc.
......ss....cCCc
......ss...cCwCc
......ss...cccc.
..cc..ss.....s..
.cCCc.ss.....s..
cCwCCcss.....s..
.cccc.ss.....s..
...s..ss.....s..
...s..ss.....s..
..gggggggggggg..
"""
GLOW_FUNGUS_COLOURS = {"c": (40, 150, 170), "C": (90, 230, 230), "w": (220, 255, 250), "s": (160, 200, 190), "g": (40, 70, 60)}
MAGMA = """
kkrkkkkkrrkkkkkk
krRrkkkrRYRrkkkr
kkrkkkkkrRrkkkrR
kkkkkrrkkrkkkkkr
kkkkrRYRrkkkkkkk
rkkkkrRrkkkkrrkk
Rrkkkkrkkkkrryrk
rkkkkkkkkkkkrRrk
kkkrrkkkkkkkkrkk
kkrRYrkkkrkkkkkk
kkkrRrkkrRrkkkkk
kkkkrkkkkrkkkrrk
kkkkkkkkkkkkrRYr
krrkkkkrrkkkkrrk
rRYrkkrRYRrkkkkk
krrkkkkrrrkkkkkk
"""
MAGMA_COLOURS = {"k": (52, 30, 26), "r": (190, 60, 20), "R": (240, 120, 30), "Y": (255, 220, 80), "y": (250, 180, 60)}
GOLD_ORE = """
sssssssssSssssss
ssSsssssssssssss
sssssyyssssssSss
ssssyYYyssssssss
ssssyyysssssssss
sSssssssssyysss.
ssssssssssyYysss
sssssssssssyysss
ssyyssssssssssss
syYYysssSsssssss
ssyysssssssssyss
ssssssssssssyYys
ssssssSssssssyys
sssssyysssssssss
ssssyYyssssssSss
sssssssssssssss.
"""
GOLD_ORE_COLOURS = {"s": (112, 112, 116), "S": (90, 90, 94), "y": (200, 160, 40), "Y": (250, 220, 90), ".": (100, 100, 104)}
GOLD_INGOT = """
................
................
................
................
................
.......oooooo...
.....ooYYYYYyo..
...ooYYYYYYyyo..
..oYYYYYYYYyyo..
..oyyYYYYYyyo...
..oyyyyyyyyyo...
..ooyyyyyyyo....
....oooooooo....
................
................
................
"""
GOLD_INGOT_COLOURS = {"o": (110, 70, 10), "y": (210, 160, 40), "Y": (250, 220, 100)}

SHIELD = """
................
..oooooooooooo..
..oWwwwwwwwwWo..
..owwwwssswwwo..
..owwwssIsswwo..
..oWwwsIIIswWo..
..owwwssIsswwo..
..owwwwssswwwo..
..oWwwwwwwwwWo..
...owwwwwwwwo...
...oWwwwwwwWo...
....owwwwwwo....
.....oWwwWo.....
......oooo......
................
................
"""
SHIELD_COLOURS = {"o": (60, 40, 20), "w": (140, 98, 56), "W": (110, 112, 120), "s": (130, 132, 140), "I": (200, 202, 210)}
PLATE = """
................
................
...oo......oo...
..oSSoooooSSSo..
..oSsSSSSSSsSo..
..oSsSSSSSSsSo..
...oSSSsSSSSo...
...oSSSsSSSSo...
...oSsSsSSsSo...
...oSSSsSSSSo...
...oSSSSSSSSo...
...ohhhhhhhho...
....oooooooo....
................
................
................
"""
PLATE_COLOURS = {"o": (40, 42, 48), "S": (170, 174, 182), "s": (120, 124, 132), "h": (140, 96, 56)}
CROSSBOW = """
................
.......o........
......owo.......
.....owwwo......
ssssssssssssss..
.....owwwo..s...
......owo..s....
......owo.s.....
......owos......
......ows.......
......owo.......
......oIo.......
......owo.......
......owo.......
.......o........
................
"""
CROSSBOW_COLOURS = {"o": (50, 32, 16), "w": (130, 88, 48), "s": (210, 205, 190), "I": (160, 164, 172)}

MINING_POST_SIDE = """
wwwwwwwwwwwwwwww
wWWWWWWWWWWWWWWw
wWWWWWWWWWWWWWWw
iiiiiiiiiiiiiiii
wWWWWWWkkWWWWWWw
wWWWWWkIIkWWWWWw
wWWWWkIkkIkWWWWw
wWWWWWWkkWWWWWWw
wWWWWWWkkWWWWWWw
wWWWWWWkkWWWWWWw
wWWWWWWkkWWWWWWw
wWWWWWWWWWWWWWWw
iiiiiiiiiiiiiiii
wWWWWWWWWWWWWWWw
wWWWWWWWWWWWWWWw
wwwwwwwwwwwwwwww
"""
MINING_POST_TOP = """
wwwwwwwwwwwwwwww
wWWWWWWWWWWWWWWw
wWyyyyyyyyyyyyWw
wWyWWWWWWWWWWyWw
wWyWkkWWWWkkWyWw
wWyWkIkWWkIkWyWw
wWyWWkIkkIkWWyWw
wWyWWWkIIkWWWyWw
wWyWWWkIIkWWWyWw
wWyWWkIkkIkWWyWw
wWyWkIkWWkIkWyWw
wWyWkkWWWWkkWyWw
wWyWWWWWWWWWWyWw
wWyyyyyyyyyyyyWw
wWWWWWWWWWWWWWWw
wwwwwwwwwwwwwwww
"""
MINING_POST_COLOURS = {"w": (110, 78, 44), "W": (150, 110, 64), "i": (90, 92, 98), "k": (40, 30, 22), "I": (170, 172, 180), "y": (200, 160, 50)}

STEEL_FRAME = """
iiiiiiiiiiiiiiii
iIIkkkkkkkkkkIIi
iIIIkkkkkkkkIIIi
ikIIIkkkkkkIIIki
ikkIIIkkkkIIIkki
ikkkIIIkkIIIkkki
ikkkkIIIIIIkkkki
ikkkkkIIIIkkkkki
ikkkkkIIIIkkkkki
ikkkkIIIIIIkkkki
ikkkIIIkkIIIkkki
ikkIIIkkkkIIIkki
ikIIIkkkkkkIIIki
iIIIkkkkkkkkIIIi
iIIkkkkkkkkkkIIi
iiiiiiiiiiiiiiii
"""
STEEL_FRAME_COLOURS = {"i": (70, 74, 82), "I": (150, 156, 168), "k": (34, 36, 40)}
MACHINE_CASING = """
iiiiiiiiiiiiiiii
iIIIIIIIIIIIIIIi
iIoIIIIIIIIIIoIi
iIIIIIIIIIIIIIIi
iIIIyyyyyyyyIIIi
iIIIykkkkkkyIIIi
iIIIykIIIIkyIIIi
iIIIykIIIIkyIIIi
iIIIykIIIIkyIIIi
iIIIykIIIIkyIIIi
iIIIykkkkkkyIIIi
iIIIyyyyyyyyIIIi
iIIIIIIIIIIIIIIi
iIoIIIIIIIIIIoIi
iIIIIIIIIIIIIIIi
iiiiiiiiiiiiiiii
"""
MACHINE_CASING_COLOURS = {"i": (60, 64, 70), "I": (120, 126, 136), "o": (40, 40, 44), "y": (190, 150, 40), "k": (30, 30, 34)}
FIRE_BRICK = """
rrrRrrrRmrrrRrrr
rRrrrRrrmRrrrRrr
mmmmmmmmmmmmmmmm
rrRmrrrrRrrrmrRr
RrrmrRrrrrRrmrrr
mmmmmmmmmmmmmmmm
rrrRrrrRmrrrRrrr
rRrrrRrrmRrrrRrr
mmmmmmmmmmmmmmmm
rrRmrrrrRrrrmrRr
RrrmrRrrrrRrmrrr
mmmmmmmmmmmmmmmm
rrrRrrrRmrrrRrrr
rRrrrRrrmRrrrRrr
mmmmmmmmmmmmmmmm
rrRmrrrrRrrrmrRr
"""
FIRE_BRICK_COLOURS = {"r": (130, 52, 36), "R": (156, 70, 48), "m": (200, 190, 170)}
HAMMER = """
................
................
...kkkkkkk......
..kIIIIIIIk.....
..kIIIIIIIk.....
..kIIIIIIIk.....
...kkkwkkk......
......w.........
......w.........
......w.........
......w.........
......w.........
......w.........
.....www........
................
................
"""
HAMMER_COLOURS = {"k": (60, 62, 68), "I": (170, 174, 182), "w": (130, 90, 50)}

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
    draw("coal_ore", ORE, COAL_ORE_COLOURS, jitter=8, seed=22)
    draw("iron_ore", ORE, IRON_ORE_COLOURS, jitter=8, seed=23)
    draw("coal", COAL_LUMP, COAL_COLOURS, jitter=6, seed=24)
    draw("iron_ingot", INGOT, INGOT_COLOURS, jitter=5, seed=25)
    draw("seeds", SEEDS, SEED_COLOURS, jitter=8, seed=26)
    draw("crop", CROP, CROP_COLOURS, jitter=10, seed=27)
    draw("ripe_wheat", RIPE, RIPE_COLOURS, jitter=10, seed=28)
    draw("wheat", WHEAT_BUNDLE, WHEAT_COLOURS, jitter=8, seed=29)
    draw("bread", BREAD, BREAD_COLOURS, jitter=6, seed=30)
    draw("ladder", LADDER, LADDER_COLOURS, jitter=6, seed=31)
    draw("door", DOOR, DOOR_COLOURS, jitter=6, seed=32)
    draw("door_open", DOOR_OPEN, DOOR_COLOURS, jitter=6, seed=33)
    draw("tool_bow", BOW, BOW_COLOURS, jitter=6, seed=34)
    draw("arrow", ARROW_ICON, ARROW_COLOURS, jitter=6, seed=35)
    draw("tool_rod", ROD, ROD_COLOURS, jitter=6, seed=36)
    draw("raw_fish", FISH, RAW_FISH_COLOURS, jitter=6, seed=37)
    draw("cooked_fish", FISH, COOKED_FISH_COLOURS, jitter=6, seed=38)
    draw("bobber", BOBBER, BOBBER_COLOURS, jitter=4, seed=39)
    draw("campfire", CAMPFIRE, CAMPFIRE_COLOURS, jitter=8, seed=40)
    draw("smoke", SMOKE, SMOKE_COLOURS, jitter=6, seed=41)
    draw("sign", SIGN, SIGN_COLOURS, jitter=5, seed=42)
    draw("tnt_side", TNT_SIDE, TNT_COLOURS, jitter=5, seed=43)
    draw("tnt_top", TNT_TOP, TNT_COLOURS, jitter=5, seed=44)
    draw("tnt_lit", TNT_SIDE, TNT_LIT_COLOURS, jitter=5, seed=45)
    draw("boat", BOAT_ICON, BOAT_COLOURS, jitter=5, seed=46)
    draw("sapling", SAPLING, SAPLING_COLOURS, jitter=8, seed=47)
    draw("pine_sapling", SAPLING, PINE_SAPLING_COLOURS, jitter=8, seed=48)
    draw("water_wheel", WHEEL_ICON, WHEEL_COLOURS, jitter=6, seed=49)
    draw("air_pipe", AIR_PIPE, AIR_PIPE_COLOURS, jitter=5, seed=50)
    draw("air_pipe_end", AIR_PIPE_END, AIR_PIPE_COLOURS, jitter=5, seed=51)
    draw("drill_side", DRILL_SIDE, DRILL_COLOURS, jitter=5, seed=52)
    draw("drill_top", DRILL_TOP, DRILL_COLOURS, jitter=5, seed=53)
    draw("drill_bottom", DRILL_BOTTOM, DRILL_COLOURS, jitter=5, seed=54)
    draw("shaft", SHAFT_ICON, SHAFT_COLOURS, jitter=5, seed=55)
    draw("compressor_side", COMPRESSOR_SIDE, COMPRESSOR_COLOURS, jitter=5, seed=56)
    draw("compressor_top", COMPRESSOR_TOP, COMPRESSOR_COLOURS, jitter=5, seed=57)
    draw("lever", LEVER, LEVER_COLOURS, jitter=4, seed=58)
    draw("lever_on", LEVER_ON, LEVER_ON_COLOURS, jitter=4, seed=59)
    draw("logic_gate_top", GATE_TOP, GATE_COLOURS, jitter=4, seed=60)
    draw("logic_gate_side", GATE_SIDE, GATE_COLOURS, jitter=4, seed=61)
    draw("signal_lamp", SIGNAL_LAMP, SIGNAL_LAMP_COLOURS, jitter=4, seed=62)
    draw("signal_lamp_on", SIGNAL_LAMP, SIGNAL_LAMP_ON_COLOURS, jitter=4, seed=63)
    draw("air_tank_side", TANK_SIDE, TANK_COLOURS, jitter=4, seed=64)
    draw("air_tank_top", TANK_TOP, TANK_COLOURS, jitter=4, seed=65)
    draw("valve", VALVE, VALVE_COLOURS, jitter=4, seed=66)
    draw("valve_open", VALVE, VALVE_OPEN_COLOURS, jitter=4, seed=67)
    draw("piston_face", PISTON_FACE, PISTON_COLOURS, jitter=5, seed=68)
    draw("piston_side", PISTON_SIDE, PISTON_COLOURS, jitter=4, seed=69)
    draw("piston_back", PISTON_BACK, PISTON_COLOURS, jitter=4, seed=70)
    draw("piston_rod", PISTON_ROD, PISTON_COLOURS, jitter=4, seed=71)
    draw("water_sensor", WATER_SENSOR, SENSOR_COLOURS, jitter=4, seed=72)
    draw("pressure_sensor", PRESSURE_SENSOR, SENSOR_COLOURS, jitter=4, seed=73)
    draw("daylight_sensor", DAYLIGHT_SENSOR, SENSOR_COLOURS, jitter=4, seed=74)
    draw("player_sensor", PLAYER_SENSOR, SENSOR_COLOURS, jitter=4, seed=75)
    draw("sensor_side", SENSOR_SIDE, SENSOR_COLOURS, jitter=4, seed=76)
    draw("sensor_bottom", SENSOR_SIDE, SENSOR_COLOURS, jitter=4, seed=77)
    draw("item_pipe", ITEM_PIPE, ITEM_PIPE_COLOURS, jitter=5, seed=78)
    draw("item_pipe_end", ITEM_PIPE_END, ITEM_PIPE_COLOURS, jitter=5, seed=79)
    draw("smelter_side", SMELTER_SIDE, SMELTER_COLOURS, jitter=5, seed=80)
    draw("smelter_front", SMELTER_FRONT, SMELTER_COLOURS, jitter=5, seed=81)
    draw("smelter_front_lit", SMELTER_FRONT, SMELTER_LIT_COLOURS, jitter=5, seed=82)
    draw("smelter_top", SMELTER_TOP, SMELTER_COLOURS, jitter=5, seed=83)
    draw("spring", SPRING, SPRING_COLOURS, jitter=6, seed=84)
    # Steel: the ingot's shape, blue-grey and brighter.
    steel = {k: (min(255, int(v[0] * 0.8 + 40)), min(255, int(v[1] * 0.85 + 50)), min(255, int(v[2] * 0.95 + 70))) for k, v in INGOT_COLOURS.items()}
    draw("steel_ingot", INGOT, steel, jitter=4, seed=85)
    draw("extractor_intake", EXTRACTOR_INTAKE, EXTRACTOR_COLOURS, jitter=4, seed=86)
    draw("extractor_out", EXTRACTOR_OUT, EXTRACTOR_COLOURS, jitter=4, seed=87)
    draw("extractor_side", EXTRACTOR_SIDE, EXTRACTOR_COLOURS, jitter=4, seed=88)
    draw("filter_pipe", ITEM_PIPE, FILTER_PIPE_COLOURS, jitter=5, seed=89)
    draw("filter_pipe_end", ITEM_PIPE_END, FILTER_PIPE_COLOURS, jitter=5, seed=90)
    draw("crafter_top", CRAFTER_TOP, CRAFTER_COLOURS, jitter=4, seed=91)
    draw("crafter_side", CRAFTER_SIDE, CRAFTER_COLOURS, jitter=4, seed=92)
    draw("crafter_bottom", CRAFTER_BOTTOM, CRAFTER_COLOURS, jitter=4, seed=93)
    draw("harvester_top", HARVESTER_TOP, HARVESTER_COLOURS, jitter=4, seed=94)
    draw("harvester_side", HARVESTER_SIDE, HARVESTER_COLOURS, jitter=4, seed=95)
    draw("drain", DRAIN_TOP, DRAIN_COLOURS, jitter=4, seed=96)
    draw("drain_side", DRAIN_SIDE, DRAIN_COLOURS, jitter=5, seed=97)
    draw("sinkhole", SINKHOLE_TOP, SINKHOLE_COLOURS, jitter=5, seed=98)
    draw("sinkhole_side", SINKHOLE_SIDE, SINKHOLE_COLOURS, jitter=5, seed=99)
    draw("pressure_gauge", GAUGE_ICON, GAUGE_ICON_COLOURS, jitter=3, seed=100)
    draw("oil_shale", OIL_SHALE, OIL_SHALE_COLOURS, jitter=6, seed=101)
    draw("tar", TAR, TAR_COLOURS, jitter=4, seed=102)
    draw("tar_side", TAR_SIDE, TAR_COLOURS, jitter=5, seed=103)
    draw("fluid_pipe", ITEM_PIPE, FLUID_PIPE_COLOURS, jitter=4, seed=104)
    draw("fluid_pipe_end", ITEM_PIPE_END, FLUID_PIPE_COLOURS, jitter=4, seed=105)
    draw("fluid_tank_side", FLUID_TANK_SIDE, FLUID_TANK_COLOURS, jitter=4, seed=106)
    draw("fluid_tank_top", FLUID_TANK_TOP, FLUID_TANK_COLOURS, jitter=4, seed=107)
    draw("derrick_side", DERRICK_SIDE.replace(".", "k"), DERRICK_COLOURS, jitter=4, seed=108)
    draw("derrick_top", DERRICK_TOP, DERRICK_COLOURS, jitter=4, seed=109)
    draw("derrick_bottom", DERRICK_TOP, DERRICK_COLOURS, jitter=4, seed=110)
    draw("refinery_side", REFINERY_SIDE, REFINERY_COLOURS, jitter=4, seed=111)
    draw("refinery_in", REFINERY_IN, REFINERY_COLOURS, jitter=4, seed=112)
    draw("refinery_out", REFINERY_IN, REFINERY_OUT_COLOURS, jitter=4, seed=113)
    draw("engine_side", ENGINE_SIDE, ENGINE_COLOURS, jitter=4, seed=114)
    draw("engine_top", ENGINE_TOP, ENGINE_COLOURS, jitter=4, seed=115)
    draw("outlet_face", OUTLET_FACE, OUTLET_COLOURS, jitter=4, seed=116)
    draw("outlet_back", FLUID_TANK_TOP, FLUID_TANK_COLOURS, jitter=4, seed=117)
    draw("outlet_side", FLUID_TANK_TOP, OUTLET_COLOURS | {"k": (30, 30, 34)}, jitter=4, seed=118)
    draw("mining_post_side", MINING_POST_SIDE, MINING_POST_COLOURS, jitter=4, seed=119)
    draw("mining_post_top", MINING_POST_TOP, MINING_POST_COLOURS, jitter=4, seed=120)
    draw("steel_frame", STEEL_FRAME, STEEL_FRAME_COLOURS, jitter=4, seed=121)
    draw("machine_casing", MACHINE_CASING, MACHINE_CASING_COLOURS, jitter=4, seed=122)
    draw("crusher_side", CRUSHER_SIDE, CRUSHER_COLOURS, jitter=4, seed=131)
    draw("crusher_top", CRUSHER_TOP, CRUSHER_COLOURS, jitter=4, seed=132)
    draw("crushed_iron", CRUSHED_IRON, CRUSHED_IRON_COLOURS, jitter=6, seed=135)
    draw("coke", COKE, COKE_COLOURS, jitter=5, seed=136)
    draw("glow_fungus", GLOW_FUNGUS, GLOW_FUNGUS_COLOURS, jitter=6, seed=137)
    draw("magma", MAGMA, MAGMA_COLOURS, jitter=8, seed=138)
    draw("gold_ore", GOLD_ORE, GOLD_ORE_COLOURS, jitter=6, seed=139)
    draw("gold_ingot", GOLD_INGOT, GOLD_INGOT_COLOURS, jitter=4, seed=140)
    draw("armour_shield", SHIELD, SHIELD_COLOURS, jitter=5, seed=141)
    draw("armour_plate", PLATE, PLATE_COLOURS, jitter=4, seed=142)
    draw("crossbow", CROSSBOW, CROSSBOW_COLOURS, jitter=4, seed=143)
    draw("fire_brick", FIRE_BRICK, FIRE_BRICK_COLOURS, jitter=6, seed=123)
    draw("hammer", HAMMER, HAMMER_COLOURS, jitter=3, seed=124)


if __name__ == "__main__":
    main()
