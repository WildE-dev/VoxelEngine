# Blocks

One file per entry, named by its id (`air.json`), holding just that entry.

```
Blocks' details, by block id. Change any of them (or leave a block out, or a
field out, for the game's own). The game loads this file again when it changes, and
the ModelEditor program edits it (its Blocks tab).
"name": what the game calls it
"textures": {"all": x}, or {"top", "bottom", "side"}, or each of "top", "bottom",
"front", "back", "left" and "right": PNGs in Resources/Textures/Blocks
"hardness": seconds to break by hand    "tool": pickaxe, axe, shovel, sword or none
"sound": soft, hard, sand, wood or glass  "light": 0 to 15
"map": its colour on the minimap
"collision": what it's bumped into, stood on, aimed at and outlined as (below)
```

## Collision

Without a `"collision"`, a solid block fills its whole cell (or its shape, if it's
chiselled or a slope), and plants, liquids and air are walked through. With one, any of:

```
"boxes": [ { "from": [x, y, z], "to": [x, y, z] }, ... ]
    boxes in the cell (0 to 1 each way) it fills instead (a pipe, a shaft, a door's
    panel), which are also what's aimed at and outlined. A block id in the list
    ("pipe") adds that block's boxes; "boxes": "pipe" on its own is the same boxes.
    At most 8 count at once. A box can count only when:
      "joined": ["+x", ...]      all of these faces join a neighbour (a pipe's arms)
      "joinedAny": ["+y", ...]   any of them do
      "joinedNone": ["+x", ...]  none of them do
      "otherwise": true          the box before it (or its run of these) didn't
"turns": true     the boxes are drawn for the block pointing +z (one with a grain,
                  along z) and turn the way it points: a lever out of the face it's on,
                  a shaft along its axle, anything else the way it faces
"mirrors": true   mirrored left to right for a door hung on its right
"solid": false    walked through, only aimed at (an open door, a lever)
"floor": false    never stood on (a door's top)
```

For example, a pipe's core and an arm out of each face where it joins the next pipe:

```
"collision": { "boxes": [
  { "from": [0.25, 0.25, 0.25], "to": [0.75, 0.75, 0.75] },
  { "from": [0.75, 0.25, 0.25], "to": [1, 0.75, 0.75], "joined": ["+x"] },
  ...
] }
```
