# Items

Things you carry that aren't blocks: ingots, food, arrows, the hammer, buckets. One
file per item, named by its id (`iron_ingot.json`), holding just that entry.

```
"name": what the game calls it
"texture": its icon, a PNG in Resources/Textures/Blocks
```

What an item leaves out keeps the game's own value. Which items there are, and what
they do, stays in the code (`src/world/Items.h`). Blocks are in `../blocks`.
