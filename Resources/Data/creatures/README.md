# Creature models

One file per entry, named by its id (`deer.json`), holding just that entry.

```
Creature models. Each creature (by its id: deer, wolf, bat, dwarf, crossbow_dwarf,
dwarf_chief, dwarf_miner, dwarf_trader, dwarf_guard,
dwarf_king, forge_golem, rock_grub) is a list of parts, drawn facing +x with its feet at 0.
The game loads them again when one changes, or edit it with the ModelEditor program.

A part is a box, and/or a frame for the parts inside it:
"box": [x, y, z]          the box's centre (in its frame)
"size": [x, y, z]         the box's size (a part with no size is just a frame)
"color": "#rrggbb"        or a name from the model's "colors"
"glow": true              lit by itself (a lamp, eyes)
"texture": "planks"       a block texture on the box, tinted by its colour, or
{"top": a, "bottom": b, "side": c}
"tile": true              the texture repeated once a block (else stretched)
"at": [x, y, z]           the frame's origin, in its parent's frame (a limb's pivot)
"rotate": [x, y, z]       the frame turned (degrees about x, then y, then z)
"swing": {"axis": "z", "amount": 1}   swung back and forth as it walks (radians at
full stride: about 0.6 times the amount)
"flap": {"axis": "x", "speed": 28, "amount": 0.7}   flapped all the time
"when": "tame"            only drawn then (a tame wolf's collar)
"parts": [ ... ]          parts inside this frame
"name": "right_arm"       its label (for animations, and the editor's tree)
A model may say "scale" (all of it bigger or smaller), "colors" (names for colours),
and "inherits": another model, whose colours and parts it starts from (a model of
the same name replacing the colour, its own parts added after). Models no creature
is called (like "dwarf_base") are only there to inherit from.
A model's "animations", by name: "attack" (played as it strikes or shoots), "work" (a
miner digging) and "idle" (all the time, if it has one), each {"length": seconds,
"loop": true or not, "parts": {label: [keys]}}, a key {"t": seconds, "rotate": [x, y,
z] degrees more, "move": [x, y, z] in its parent's frame}, eased from key to key.
```
