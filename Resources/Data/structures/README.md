# Structure models

One file per entry, named by its id (`oil_derrick.json`), holding just that entry.

```
Multiblock structures' models (world/Multiblocks.h), by structure: oil_derrick,
refinery, blast_furnace,
diesel_compressor, coke_oven. Same format as creatures.json, in the structure's own frame:
x and z run 0 to 3 across its 3 x 3 footprint (as its pattern's cells, before it's
turned to face the way it was built), y up from the ground under it. Parts with
"when": "working" are drawn only while it works ("idle": only while it doesn't), and
flaps only move while it works. Leave its ports' cells clear: they stay real pipes.
Boxes can wear block textures ("texture", "tile"): see creatures.json. The
ModelEditor program edits this file. The game loads them again when one changes.
```
