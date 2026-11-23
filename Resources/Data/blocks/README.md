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
```
