# Bakes the UI font (Aileron Regular, dotcolon.net, free for any use; the copy that
# Pillow carries) into Resources/Fonts/ui.png (white glyphs, coverage in alpha) and
# Resources/Fonts/ui.json (where each glyph is and how it's placed). Printable ASCII,
# plus a box for anything else; a small white square at the atlas' corner is what the
# UI draws solid shapes with (so text and shapes share one texture).
#   python tools/bake_ui_font.py
import json
import os
from PIL import Image, ImageDraw, ImageFont

SIZE = 32          # pixels: drawn smaller (or larger) by scaling, with filtering
ATLAS = 512
PAD = 2
here = os.path.dirname(os.path.abspath(__file__))
out_dir = os.path.join(here, "..", "Resources", "Fonts")
os.makedirs(out_dir, exist_ok=True)

font = ImageFont.load_default(size=SIZE)
ascent, descent = font.getmetrics()
atlas = Image.new("L", (ATLAS, ATLAS), 0)
draw = ImageDraw.Draw(atlas)
# The solid white square for shapes.
draw.rectangle([0, 0, 3, 3], fill=255)

glyphs = {}
x, y, row = 8, 0, 0
for code in list(range(32, 127)) + [0x25A1]:  # (U+25A1: the box drawn for anything missing)
    ch = chr(code)
    if code == 0x25A1:
        w, h = SIZE // 2, ascent - SIZE // 6
        bbox = (0, ascent - h, w, ascent)
        advance = w + SIZE // 6
    else:
        bbox = font.getbbox(ch)  # (left, top, right, bottom) from the origin at the top of the ascent
        advance = font.getlength(ch)
        w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
    if x + w + PAD >= ATLAS:
        x, y = 0, y + row + PAD
        row = 0
    if w > 0 and h > 0:
        if code == 0x25A1:
            draw.rectangle([x, y, x + w - 1, y + h - 1], outline=255, width=2)
        else:
            draw.text((x - bbox[0], y - bbox[1]), ch, font=font, fill=255)
    glyphs[str(code)] = [x, y, w, h, bbox[0], bbox[1], round(advance, 3)]
    x += w + PAD
    row = max(row, h)

rgba = Image.new("RGBA", atlas.size, (255, 255, 255, 0))
rgba.putalpha(atlas)
rgba.save(os.path.join(out_dir, "ui.png"))
with open(os.path.join(out_dir, "ui.json"), "w", newline="\n") as f:
    json.dump({"size": SIZE, "ascent": ascent, "descent": descent, "atlas": ATLAS, "white": [1, 1], "missing": 0x25A1, "glyphs": glyphs}, f)
print("baked", len(glyphs), "glyphs; ascent", ascent, "descent", descent)
