# VoxelEngine

A small OpenGL voxel engine with sloped terrain, caves, trees, chunk streaming on a
background thread, block lighting, ambient occlusion, finite flowing water and a
skybox. Deer roam it and ghouls come out in the dark. Several players can share a
world over the network. (A black hole lensing
effect at spawn is turned off; `kDrawBlackHole` in `src/render/Renderer.cpp` brings it
back.)

## Building

Dependencies come from [vcpkg](https://vcpkg.io) (manifest mode). With `VCPKG_ROOT` set:

```bash
cmake --preset default
cmake --build build
cd build && ./VoxelEngine
```

The executable loads `Resources/` relative to the working directory, and the
build copies it next to the binary. Run with no options, it opens at the title
screen (see [Menus and settings](#menus-and-settings)). With options, such as
`--world`, `--connect` or `--server`, it goes straight into that game.

The build also produces `VoxelServer` (a dedicated server that doesn't link GL or
GLFW), `NetSmokeTest` (see [Multiplayer](#multiplayer)), `SaveTest` (see
[Saving](#saving)), `WaterAreaTest`, `PumpTest` and `EvaporationTest` (see [Water](#water)),
`WeatherTest` (see [Day, night and weather](#day-night-and-weather)), `ChatTest`
(see [Chat and commands](#chat-and-commands)), `DungeonTest` (see
[Dungeons](#dungeons)), `TrapTest` (see [Health and traps](#health-and-traps)),
`ShapeTest` (see [Block shapes](#block-shapes)), `BiomeTest` (see [Biomes](#biomes))
`ModeTest` (see [Game modes](#game-modes)), `MiningTest` (see [Mining](#mining)),
`BreathTest` (see [Breathing](#breathing)), `CreatureTest` (see
[Creatures](#creatures) and [PvP](#pvp)), `CraftTest` (see [Crafting](#crafting)),
`FoodTest` (see [Food and hunger](#food-and-hunger)), `ChestTest` (see
[Chests](#chests)), `BedTest` (see [Beds](#beds)) and `SoundTest` (see
[Sound](#sound)).

`ctest` in the build directory runs them all, and `ctest -j15` runs them all at
once. They're written to pass even then, on a loaded machine:

- They wait for things to happen rather than for fixed times.
- Where the client shows a change before the server makes it (crafting, for
  example), they wait for the server's answer before checking.
- Their servers and clients use a 4-chunk view distance, about 27 times less terrain
  than the full 12, so sixteen at once don't starve each other.
- The network layer also rides out stalls of many seconds: ENet's timeout is raised
  from 5 s to at least 20 s, with larger socket buffers so bursts of chunks aren't
  dropped.

## Multiplayer

Every game is a client of a server. For a singleplayer world, the game runs a
server inside the same process on a free port of the loopback interface and
connects to it, so singleplayer and multiplayer use the same code. Join a server
from the title screen's Multiplayer page, or from the command line as below.

Host a dedicated server (no window; Ctrl+C stops it):

```bash
cd build && ./VoxelEngine --server
```

`--view-distance N` (2 to 12 chunks, default 12) sets how far around each player
the server loads and sends terrain. Less is much lighter on a small machine.
Players who ask for less, in their settings, get less.

`./VoxelEngine --server 30000` picks another port (the default is UDP 27450), and
`./VoxelServer [port]` does the same without any graphics libraries. Others join
with:

```bash
cd build && ./VoxelEngine --connect 192.168.1.20 --name Alice
```

`--connect host:port` sets the port. To play on the same machine as the server,
connect to `127.0.0.1`. Over the internet, forward UDP port 27450 to the server.
Other players appear as box figures in their own colour, with a dark visor on the
side they're facing. Each has a right arm holding whatever is in their hand: a block
as a little textured cube, a tool or item as its icon. Survival players' armour shows
over them (see [Swords and armour](#swords-and-armour)). Their name floats over their
head, within 40 blocks and only when nothing's in the way. The server sends names once
(when someone joins), and what's in hand and the armour with every position update.
The server saves the world (see [Saving](#saving)).

`NetSmokeTest` starts a server and two clients in one process (real sockets on
localhost, no windows) and checks that chunks stream, edits and flowing water
reach both clients identically, players see each other, and a flood stays cheap
on the wire. `./NetSmokeTest host:port` runs the same checks against a running
server.

### How it works

- **Server** (`src/server`): owns the world. Its worker thread generates terrain
  and propagates light, the water simulation runs on its own thread, and the
  server thread applies edits and streams the world at 20 ticks a second. It
  loads chunks within view distance of any player.
- **Client** (`src/client`): `ClientWorld` holds the chunks received from the
  server and meshes them on a worker thread; it never generates, lights or
  simulates anything itself. `Client` handles the connection.
- **Protocol** (`src/net/Protocol.h`): one message per packet over
  [ENet](http://enet.bespin.org) (reliable UDP). World data goes on a reliable,
  ordered channel; player states and snapshots on an unreliable, sequenced one.
- **Chunks** go to each client nearest first, once generated and lit. Blocks and
  light are split into byte planes (type, the four slope edges, water, drawn water,
  light), each run-length encoded in layer order: an empty or solid chunk is about
  30 bytes, and joining costs about 3.5 MB for the 15,000 chunks in view.
- **Changes** are batched per client and per chunk until the next send, so a cell
  that changes several times in between goes out once with its latest value.
  Flowing water is only sent when its drawn surface moves (most of what water does
  is small volume changes within the same surface height), nearest chunks first
  within a byte budget per tick, and a chunk with hundreds of changed cells is
  re-sent whole. A flood of about a hundred water blocks costs each client around
  30 KB/s. When light changes, the chunk's new light is sent.
- **Movement** is predicted on the client with the usual collision and sent 20
  times a second. The server checks it loosely (no faster than falling at terminal
  velocity, not inside solid blocks for long) and otherwise moves the player back
  to its last good position. Others are shown 100 ms in the past, interpolated
  between snapshots.
- **Edits** are applied on the client at once and sent to the server, which checks
  reach and that solid blocks don't go inside a player. It answers with the real
  block either way, so a refused edit is undone.

## Saving

The server saves the world to a directory: `world/` for a dedicated server, and
`saves/<name>/` for singleplayer worlds made from the title screen. `--world`
from the command line defaults to `saves/singleplayer/`. Both are relative to the
working directory.
`--world DIR` picks another one, and `--no-save` neither loads nor saves anything.
Works the same with `VoxelEngine` and `VoxelServer`:

```bash
cd build && ./VoxelServer --world worlds/survival
```

For testing, `--empty-world` makes a new world that's a flat, empty plain: grass over
dirt over stone, with no hills, caves, water, trees or plants. It gets its own default
save (`saves/empty/`, or `world-empty/` with `--server`), and the save remembers its
type, so it reopens empty even without the flag. An existing world always keeps the
type it was created with.

```bash
cd build && ./VoxelEngine --empty-world
```

It autosaves every minute and saves when it stops (Ctrl+C, or quitting
singleplayer). Players are remembered by name, so rejoining with the same
`--name` puts you back where you left, facing the same way (and still flying if you
were). If that spot has been built over since, you land on the surface instead.

- **Only edited chunks are stored.** Generation is deterministic, so untouched
  terrain is simply generated again. An edited chunk is saved when it leaves every
  player's range and on each save. Saved chunks load in place of generation.
- **Light isn't stored**; it's recomputed as chunks load. While the chunk above
  a column isn't loaded, its sky light is estimated from the generator, which
  only knows untouched terrain, so saved chunks also record which columns they
  block. A roof you built stays dark underneath even before its chunk loads.
- **Water** that was still moving when it was saved carries on once its chunk
  loads.
- **Format** (`src/server/WorldStorage.h`): `world.dat` holds the format version,
  the world type, the time of day, the default game mode and the players (with their
  game modes and survival resources); `regions/r.X.Y.Z.vxr` holds the stored chunks of an 8x8x8-chunk
  region, as the same run-length encoded block planes the network uses (an edited
  surface chunk is a few KB). Each file is written to a temporary file and renamed,
  so a crash mid-save leaves the previous save intact. A damaged file, or one from
  a newer format, stops the server from starting rather than being skipped and
  then overwritten. Saves from older formats still load (region files from before
  blocks had an axis are read with every log upright).

`SaveTest` saves and reloads a world in a temporary directory. It checks that
edits come back, that chunks are saved when they unload, light under a saved roof,
that damaged files are refused, and that a rejoining player is put back in place.

## Controls

| Key | Action |
| --- | --- |
| WASD | Walk (or fly) |
| Space | Jump (swim: up; fly: up) |
| Shift | Sprint (fly: faster) |
| Left Ctrl (hold) | Crouch: lower (fits under 1.5-high gaps) and slow, and you won't walk off edges; your name tag hides. You stand again once there's room. (Flying: down) |
| Q / E | In no-clip (`/noclip`, operators): fly down / up |
| Mouse | Look (a crosshair marks the middle of the screen) |
| Left click | Break block (survival: hold to mine it), or hit the creature in front of you |
| Right click | Place the selected block; eat food in hand; open a chest; use a bed (Shift: place against a chest or bed instead) |
| V / Shift+V | Placement height for building blocks: full, 3/4, 1/2, 1/4 (and back) |
| 1-9, 0 / mouse wheel | Select the hotbar slot to place from (with Tab you can also click a slot) |
| I | Inventory: pick which tool or block goes in each hotbar slot |
| C / Shift+C | Chisel: lower / raise the quarter of the block you're looking at by a quarter |
| L (hold) | Laser, operators only: carve a tunnel along the view ray |
| R | Reload shaders and block textures, rebuild all chunk meshes |
| Tab | Toggle mouse capture |
| F1 | Toggle wireframe |
| F2 | Toggle debug window |
| F3 | Toggle ambient occlusion |
| F12 | Save a screenshot (`screenshot-<date>-<time>.png` in the working directory) |
| N | Skip to the next dusk (by day) or dawn (at night), for everyone on the server |
| T / Enter | Open chat (Enter sends, Esc closes). While it's open the view stays still and the cursor is free |
| / | Open chat with a command started |
| Esc | Pause menu: resume, settings, back to the title screen, quit (or close the chat) |

## World generation

`TerrainGenerator` builds each chunk independently and deterministically:

1. **Surface:** a heightmap from 8 octaves of Perlin noise, snapped to eighths of
   a block, plus mountain ranges, with swamps sunk to sea level (below). Each column
   gets a sloped block on top from its biome (below): grass, dry or lush grass, snowy
   grass or desert sand over dirt or sand; beaches by the sea, bare rock high up and on
   steep slopes, and snow on the peaks; stone underneath.
2. **Caves** (`Caves.cpp`): winding tunnels where two 3D noise fields are both
   near zero, plus large caverns more than 24 blocks down. Tunnels that reach
   the surface form entrances. Noise is sampled every 4 blocks and interpolated.
3. **Features** (`Features.cpp`): trees cluster in forests, plus bushes and
   boulders. Each kind sits on a jittered grid, so a chunk can generate the parts
   of features rooted in neighbouring columns.
4. **Plants:** tall grass and flowers, drawn as crossed sprites that sit on the
   sloped surface. Breaking the block under a plant removes it.
5. **Dungeons** (`Dungeons.cpp`): see below.

Open space below sea level (row -3) becomes seas and lakes, with sandy beaches
and sea floors. Caves never open under water, so they stay dry.

### Biomes

Every column has a climate: a temperature and a humidity, each from a very
large-scale noise field (so a biome spans hundreds of blocks), with temperature
falling the higher the ground. Where it falls between the two decides its biome:

| | dry | | wet |
| --- | --- | --- | --- |
| **freezing** | Tundra | | Snowy taiga |
| **cold** | Taiga | Taiga | Taiga |
| **temperate** | Plains | Forest | Swamp |
| **hot** | Desert | Savanna | Jungle |

- **Plains:** open grass full of flowers, a tree here and there.
- **Forest:** oak woods. Spawn is in one.
- **Swamp:** lowland sunk to sea level, so the sea fills its hollows with shallow standing
  water among lush grass and muddy beds.
- **Desert:** sand, cacti, dead bushes, no trees.
- **Savanna:** dry yellow grass with flat-topped trees far apart.
- **Jungle:** lush grass, thick undergrowth, and tall big-crowned trees close together.
- **Taiga:** pine forest.
- **Snowy taiga, Tundra:** snow-covered ground (dense pines, or open with the odd stunted
  one), only dead bushes poking through, and seas frozen over (their top layer is ice,
  with water underneath).

Mountains climb out of any of them into bare rock and snow (below). Biome borders
change gradually, since the climate does. The debug window (F2) shows the biome where
you stand, with its temperature and humidity. New blocks: dry, lush and snowy grass,
pine needles, cactus, dead bush and ice (in the inventory). `BiomeTest` checks that
every biome turns up with a fair share, spawn is temperate, climate changes smoothly
and cools with height, and, deep inside each biome, that it has its ground and plants
(cacti but no trees in deserts, pines in taigas, many more trees in jungles than on
plains), frozen seas have ice over water, swamps have shallow water, and nothing
floats. Chunks generate in about 0.2 ms.

### Mountains

Ranges rise out of the hills in about a fifth of the world: a very large-scale noise
field decides where they run, and ridges inside them (rounded at the crests, with
smaller ridges on their flanks) climb up to about 90 blocks above the hills, so peaks
reach around 100. Above row 44 the ground is bare rock, with nothing growing (trees,
bushes, grass and flowers stay below that tree line); above row 70 it's snow. The
slopes are kept mostly under 45 degrees, since each column's surface is one sloped
block (about 1% of mountain columns are steeper, and show a step). The nearest big
range to spawn is about 700 blocks out, towards +x +z.

### Dungeons

Deep underground (their floors 43 to 52 blocks down, well below any sea floor) lie
dungeons of brick rooms joined by corridors. The world is divided into 128 x 128
regions and about three in ten hold one: 7 to 12 rooms on one level, linked by a
spanning tree of 3-wide corridors plus a few extra loops, with lamps set into the
corridor walls. Each dungeon's first room is its entrance: a spiral staircase climbs
a shaft from it to the surface, marked by a ring of bricks on the ground (unless the
entrance is under the sea or a beach). Otherwise you'll have to dig.

The other rooms work through the room types in a shuffled order, so a dungeon only
repeats one once it has used them all:

| Room | |
| --- | --- |
| Pillared hall | a tall room with rows of pillars crowned with lamps |
| Archive | shelves (logs) along the walls, a table under a hanging lamp |
| Pool | a sunken pool of water |
| Overgrown garden | grass floor, leafy bushes, tall grass and flowers, mossy walls |
| Crypt | a low room with rows of stone tombs |
| Vault | clean bricks, lamps set into the floor around a lit pedestal (ringed by a trap) |
| Collapsed room | heaps of stone, dirt and sand |
| Cistern | a deep pit of water with pillars, a ledge all round |
| Pump room | a working pump lifting water from a sunken basin up a pipe into a raised tub |
| Shrine | a lamp on a plinth, pews facing it |
| Flood chamber | a pressure plate in the floor opens sluice gates in the ceiling, and the reservoir above pours in |

**Traps.** Some long corridors have a spike pit across them: either hidden under
crumbling bricks (three long; they're cracked, if you look) or open (two long, so you
can jump it). A vault's lit pedestal is ringed by crumbling floor over spikes. Fall in
and there's no climbing out. Flood chambers flood when you step on their plate.

**Loot.** Dungeons hide chests: the prize on every vault's pedestal (past the trap),
one by each archive's reading table, grave goods at the end of a crypt's aisle,
offerings behind most shrines' plinths, and now and then one in a corner of a hall or
half buried in a collapsed room. A chest holds three to five finds (food, hide, lamps,
planks, glass, bricks, pipes, logs); a vault's holds five to seven richer ones (more
food and hide, lamps, pumps, sluice gates, glass, pipes, spikes, even a spare chest or
a bed). The loot is rolled from the chest's position and room the first time anyone
opens or breaks it (`Dungeons::LootFor`), so it's the same whoever finds it, and kept
from then on like any chest's.

Layouts come from a seed hashed from the region's coordinates, and everything a
dungeon places is a function of world position, so each chunk stamps just its own
part and neighbouring chunks agree at their borders. Stamping runs in three passes:
brick shells, then the open space (so a corridor opens a doorway where it meets a
room), then furnishings, which never read neighbouring blocks (they might be in a
chunk that isn't generated). Raised furniture keeps out of corridors (one can run
straight through a room), water sits in sealed basins, and a pump room's pump starts
working once a player is near. It costs little: chunks still generate in about
0.15 ms. New blocks: stone bricks and mossy bricks (on the hotbar after 0; pick them
with the mouse wheel). Empty worlds (`--empty-world`) have no dungeons.

`DungeonTest` lays out 225 regions and generates 16 dungeons chunk by chunk. It checks
that every room type turns up, layouts and chunks are deterministic, every room can
be reached from the entrance across chunk borders, shafts reach the surface, water
can't spill, rooms stay deep, gardens and archives are furnished all over regardless
of chunk borders, pump rooms are wired up, and vault and archive chests are there with
loot in them (and none for a chest outside a dungeon).

## Water

Water is drawn in its own pass after solid geometry and the sky (`water.vert`,
`water.frag`). The scene behind it is copied first, and the surface shows that
copy bent by small moving waves (refraction), fading to blue-green with the
amount of water the light passes through (red goes first). It reflects the sky,
more at grazing angles (Fresnel), dimmed in caves and at night, and catches a
glint of the sun. From below you mostly see through the surface, and the screen
tints blue when your head is underwater. You swim in it: you sink slowly, Space swims up, and swimming into
a ledge with Space held climbs out.

Water is finite and flows (`Water.cpp`). Each cell holds up to 128 units of
water: water blocks, and sloped blocks whose open space above the slope can be
waterlogged. Water is only ever moved, never created or destroyed (apart from
resting puddles evaporating; see below). It falls into
any room below it, otherwise it levels out by height with nearby water. A cell
over a drop counts as lower, so water pours over edges in big shares and falls
as a continuous stream. A bucket poured on flat ground spreads into a shallow
puddle and stops, and digging next to a lake lowers it slightly as the hole
fills.

While water moves, the drawn surface hides small back-and-forth changes (it
follows the real level once that has moved an eighth of a block, and snaps to it
once the water settles), and surfaces are drawn in 32nds of a block, so a
draining lake goes smoothly down instead of jittering. Surface corners average
the water height of neighbouring columns across rows, so flowing water slopes
between levels, thin edges taper to the ground, and a waterfall's curtain meets
the water pouring over its brink.

The simulation runs on the server, on its own thread, up to 65,536 water cells per tick, four
ticks a second (water spreads a cell a tick, so it flows at a calm pace, and a slow
tick leaves room for big floods), and only near something that changed. It holds a world-wide edit
lock in short slices and steps aside for waiting edits, so player edits never
wait more than a couple of milliseconds. Place a full block of water with 8.

**Pumps and pipes** (hotbar 9 and 0) move water anywhere, including uphill. Each
tick a pump takes 5/8 of a block of water (2.5 blocks a second) from the cells
touching it and sends it through the pipes joined to it, face to face, out of the
network's open ends: the open cells beside any pipe that joins only one other pipe
or the pump. The ends share the water equally, and the pump only takes what they
have room for, so water is still never created or lost. A pump draws only from the
cells it touches, so put it at the bottom of a pool to empty it. Pumps run within
the water simulation distance, like flowing water, and keep running after a save
and reload. `PumpTest` checks the rate, that water is conserved, that pumps stop
when the pipe is cut or the outlet is full, and that saved pumps carry on.

**Water on water** (a lake's top layer, or a pool topped up) comes right down to the
water below wherever it meets an empty column over that water, so a thin layer thins
out to nothing at its edges instead of ending in a little wall. As a lake drains, its
last layer shrinks smoothly rather than breaking into patches.

**Thin water**: while water moves, its drawn height follows the real volume in steps
of an eighth of a block (which hides the jitter of flowing water), but below an eighth
it follows exactly, and water lying on more water (the top layer of a lake or pool)
is sent to clients down to a 32nd. So a draining lake's last layer thins out evenly
instead of holding at an eighth and vanishing cell by cell. A thin film on dry ground
still ignores changes under an eighth, so puddle edges don't flicker.

**Rain** tops up bodies of water. While it rains, the same random ticks that dry up
puddles look for water: when one lands in a body of water, the top water cell of
that column gets a 16th of a block more, if it's open to the sky, isn't full and has
water under it. So rain refills drained lakes and pools to the level they had, deep
water (hit more often) fastest, and never puts water in a dry cell, above a full one,
or in a puddle on dry ground. Puddles don't evaporate while it rains.

**Evaporation**: shallow puddles that have stopped flowing dry up. Every water tick,
8 random cells of each chunk near a player are looked at (random ticks), and a resting
puddle among them disappears: a cell with at most a quarter of a block of water that
isn't full, has no water above it and isn't flowing. Each cell comes up about every
2.1 minutes on average, so a spill fades away cell by cell. This is the only way
water is ever destroyed; lakes and pools are deeper than that and stay. Random ticks
cost about 1 ms per tick for the whole simulation area. `EvaporationTest` checks that a
spill settles into a puddle that dries up gradually, that a deep pool keeps its water,
and that nothing evaporates with no player nearby.

Water only flows within 6 chunks of a player (per axis), half the view distance of
12, so a large world doesn't spend CPU on floods nobody is watching. Further out,
moving water pauses where it is and carries on when a player comes back within
range. `--water-distance N` changes it on `VoxelEngine` (singleplayer or
`--server`) and `VoxelServer`; it's kept below the view distance, because water
needs loaded chunks around it to flow into. `WaterAreaTest` checks that water
pauses out of range, keeps its volume, and resumes.

## Player

You walk on the terrain with gravity and collision. The body is a 0.6 x 1.8
box. Full blocks collide as boxes. Sloped terrain blocks collide as a height
field that matches their rendered triangles, so you stand on the visible
surface and walk up and down hills smoothly. You can step up ledges up to 0.6
blocks; anything taller needs a jump (about 1.25 blocks high). Physics runs at
a fixed 120 Hz. Until the chunks under you have loaded, you wait in place. Fly
mode (F) moves freely through blocks like the old free camera.

## Block shapes

A block can be shaped by its four quarters, each 0 to 8 eighths high (the terrain's
sloped tops are shaped this way). How a shape is drawn, and stood on, depends on the
block: natural ground (grass, dirt, sand, stone, snow) slopes smoothly between its
four corners, while anything built (bricks, logs, lamps and the rest) is four
quarter-columns, each as high as its corner, like small steps.

Shape blocks yourself with the chisel: **C** lowers the quarter you're looking at by
a quarter of a block, **Shift+C** raises it again. Raising every quarter to the top
makes the block whole; lowering them all removes it. Machines (pumps, pipes, sluice
gates, pressure plates), water and plants can't be chiselled. Shaped blocks hold
water above their quarters like the terrain's slopes do, and fill their lowest parts
first. The water treats a cell as four columns: a stepped block's quarters, or a
slope's corners weighted like its two triangles. So a notch carved down to the floor
in one quarter takes water from a pool beside it, even though the block as a whole
stands higher than the pool's surface. Water only crosses a carved block through
quarters lower than the water, though. It gets in by an open quarter on the side it
comes from, and goes on through only along open quarters joined side by side. A solid
face higher than the water blocks it, and two notches meeting at a corner aren't a
channel. Water in a carved block is drawn flat at its own level, with the block's
higher parts standing out of it.

Every block is drawn inside its own cell: where a slope climbs past the top of its
block, its corner is clipped at the block's top and the column beside it carries on
in the row above. The top of a slope or slab with a block sitting on it is shaded as
in that block's shadow, lit by the open space in its own cell.

The outline around the block you're looking at follows its shape (a slope's corners
and fold, a stepped block's quarters, a pressure plate's slab), and blocks in front of
it hide it. Aiming follows shapes too: looking over a slope or past a quarter-column
targets the block behind it, so you can break and build through the open part of a
partial block.

`ShapeTest` checks standing heights and water for both styles, aiming past and into
partial blocks, the outlines' shapes, chiselling (down to
nothing and back to whole), that the mesher draws quarter-columns, joins slopes
across rows and tapers thin water to nothing with no walls, chiselling over the
network, and the mountains: high snowy peaks, bare rock with nothing growing above the
tree line, gentle enough slopes, and that a player spawning on a peak lands on top.

### Logs

Logs lie along the face you place them against. Against the top or bottom of a
block they stand upright. Against a side they lie on their side, pointing away from
that face. The rings of the end grain show on their two ends, and the bark's grain
runs along the log. That holds for log slabs (V) too, and chiselling keeps the
direction. Carving into a log shows the wood inside (`log_inside.png`). A
quarter's top cut below the log's, and the step from it up to a taller quarter,
show light wood with its grain along the log. The outside keeps its bark and end
grain. A log slab's top counts as cut too. The direction is part of the block (`Block::axis`), so it's sent to other
players and saved with the world. `ShapeTest` checks placement, which texture each
face gets, the direction of the bark, which carved faces show the inside wood,
encoding, and that another player sees a sideways log.

## Inventory

The hotbar holds ten tools or blocks (keys 1-9 and 0). Press I to open the
inventory: the tools along the top (see [Mining](#mining)), then every block you can
place (in survival, the ones you have), in a grid. Click one to put it in the selected hotbar slot; pick
the slot with the number keys or by clicking it. I or Esc closes it. Your hotbar is
kept in `hotbar.txt` in the working directory, so it's the same next time.

## Game modes

There are two, and each player is in one:

- **Survival** (the default): you build with what you collect. Breaking a block
  gives you its worth in that block type, and placing one costs it. Traps hurt, and
  you have hearts. You can break and build up to 6 blocks away.
- **Creative**: every block, as many as you like, and nothing hurts you. You can
  break and build up to 50 blocks away.

The server checks the reach too (with a little slack for movement).

A full block is worth 64 units. Shaped blocks are worth their volume: a half slab is
32, a quarter slab 16, and a chiselled block whatever's left of it (a slope its mean
height). Press V to pick the height you place building blocks at (full, 3/4, 1/2,
1/4; Shift+V goes back up). The hotbar shows the selected item's name and, for building
blocks, the height they're placed at; it's reddish when you can't use it (not enough of a block, or a tool not made yet), and how much you
have on each slot. Counts are shown in blocks, so 32 units reads as 0.5. Chiselling a
block down gives back what it takes off, and building it up costs the same.
Plants, pumps, pipes, sluices and pressure plates can't be shaped, so they're
always worth a whole block. Pebbles are the exception. They're small clusters of
stones lying about on the ground, more often on beaches and bare rock, and they're
picked up at once with a click. Each one counts as a quarter of a block of stone
(16 units), and putting pebbles down costs the same. Water can't be collected, so in survival it can't be
placed either; use pumps to move it. The laser gives nothing.

In survival the inventory (I) lists only the blocks you have, with counts on them
and on the hotbar slots; a slot you've run out of is dimmed. The server keeps
everyone's mode and resources and saves them with the player. It refuses an edit you
can't afford, so a modified client gets nothing for free.

A new world starts players in survival, or in creative with `--empty-world`, since
that world is for testing. Choose with `--mode creative|survival` (for `VoxelEngine`
and `VoxelServer`). The world remembers the mode it was made with. Operators change a
player's mode with `/gamemode <creative|survival> [player]`, and hand out blocks
with `/give <block> [count] [player]`, for example `/give stone_brick 32`.

`ModeTest` checks:

- what blocks are worth;
- that breaking gives and placing costs, at each height;
- that what you can't afford isn't placed;
- that chisel refunds and costs balance;
- that spikes hurt in survival and not in creative;
- that `/gamemode` and `/give` are for operators;
- that modes and resources survive a restart.

## Health and traps

In survival you have ten hearts, shown above the hotbar (creative has none). The server keeps track of them, so traps
work the same for everyone:

- **Spikes** cost two hearts every half second you stand in them.
- **Crumbling bricks** give way 0.4 seconds after you stand on them, and the collapse
  spreads to the crumbling bricks touching them, so a whole pit cover falls in.
- **Pressure plates** open every sluice gate within 8 blocks.

The screen flashes red when you're hurt. Hearts come back one every 4 seconds once
nothing has hurt you for 5, as long as you're well fed (see
[Food and hunger](#food-and-hunger)). With none left, everyone hears how you died and you
respawn at the spawn point (or your bed: see [Beds](#beds)) with full health (and three seconds' grace). In survival,
half of every kind of block and item you carry (rounded down) is left behind in a
chest placed where you died (or just beside or above, in the nearest open cell), and
the chat tells you where; anyone can open it, so a killer can take it. Your tools and
armour aren't dropped. In no-clip
nothing can hurt you. All four blocks are in the inventory, for building traps of
your own. `TrapTest` checks spikes, death and respawn, crumbling floors, pressure
plates and sluice gates, and that no-clip and the laser are operators' only.

## Mining

In survival, breaking a block takes time: hold the left button on it, and cracks
spread across it until it breaks. Looking away, or switching tools, starts over.

| Block | By hand | With the right tool (4x faster) |
| --- | --- | --- |
| Plants | at once | |
| Leaves | 0.3 s | |
| Dirt, grass, sand, snow, cactus | 0.6 s | shovel (cactus: axe), 0.15 s |
| Ice, lamps, crumbling bricks | 1 s | pickaxe, 0.25 s |
| Logs | 2 s | axe, 0.5 s |
| Spikes, pressure plates | 2 s, 1.5 s | pickaxe |
| Pumps, pipes, sluice gates | 3 s | pickaxe, 0.75 s |
| Stone | 4 s | pickaxe, 1 s |
| Stone bricks | 5 s | pickaxe, 1.25 s |

A carved block takes time in proportion to what's left of it, but never less than
30%: a half slab takes half as long. The wrong tool is no better than a hand. The
pickaxe, axe and shovel are in the inventory's top row (I). In survival you make them
first (see [Crafting](#crafting)). Put one in a hotbar slot to hold it. A tool places
nothing on a right click. In creative everything breaks at once, as before.

The server times every break, so a modified client can't mine faster. The client
says when it starts on a block and with which tool. The server lets the break through
only once that block has been mined for long enough with that same tool, give or take
0.2 s for network jitter. The rules are in `src/world/Mining.h`. `MiningTest` checks:

- the times and tools;
- that breaks which weren't mined, or weren't mined for long enough, are refused;
- that the tool and the block must match what was started;
- that creative is instant.

## Creatures

Two kinds of creature live in the world. The server runs them and the players near
them see them, smoothed like other players.

- **Deer** wander grassland in herds of one to three. Hit one and it's knocked back
  and bolts away from you for a few seconds.
- **Ghouls** come out where it's dark: light 4 or less, with sky light counting for a
  quarter at night. That means caves, dungeons and the surface after dusk. One goes
  for the nearest survival player within 16 blocks and hits for one and a half
  hearts a second while in reach. Creative players, and players in no-clip, are left
  alone. Daylight under the open sky burns them up in about six seconds.

Left click a creature in front of you (within 4.5 blocks) to hit it. A hand does
1 heart, a shovel 1.5, a pickaxe 2, an axe 2.5 and a sword 3.5. Hits land at most about three
times a second, and the server checks the reach. A deer has 4 hearts and a ghoul 6.
Aiming at a creature doesn't mine the block behind it.

In normal worlds they spawn by themselves around each player:

- deer on open grass 20 to 48 blocks away, up to 6 nearby;
- ghouls in the dark within 40 blocks, up to 5 nearby, never closer than 12.

They go when no player is within 96 blocks. They aren't saved: a world fills with
them again as you play. Empty worlds have none of their own; `/summon deer` or
`/summon ghoul` brings one anywhere. Their models are boxes like the players': a deer
with antlers, and a ghoul with outstretched arms and glowing eyes. Their legs swing as
they move, and they flash red when hit. The rules are in `src/world/Creatures.h` and
the server side in `src/server/Creatures.cpp`.

`CreatureTest` checks:

- summoning, and that both players see creatures fall onto the ground;
- hits: out of reach, too fast, knockback and fleeing, kills;
- a ghoul hunting a survival player and ignoring a creative one;
- burning at noon, and `/creatures clear`;
- in a normal world, deer turning up by day and ghouls at night.

## PvP

Players can hit each other: left-click another player in reach. Damage, reach and
the time between hits are the same as for creatures (a heart bare-handed, 2.5 with an
axe). The player hit flashes red for everyone else, with a thump, and is knocked back
away from the hitter and a little up. The server sends the push and the player's own
game applies it, so the usual movement checks still hold. Killing someone tells
everyone (`X was slain by Y`), and they respawn. Creative players and players flying in
no-clip can't be hurt. PvP is on by default; operators switch it with `/pvp on|off`,
and `/pvp` shows which. With it off, a hit just tells you so. Someone slain leaves
half of what they carried in a chest where they fell (see
[Health and traps](#health-and-traps)). `CreatureTest` checks
names, held items and armour reaching other players, reach, damage, the red flash,
knockback, `/pvp off`, being slain, and the killer finding half the victim's things in
a chest where they died.

## Swords and armour

The **sword** is a tool made for fighting: 3.5 hearts a hit, against creatures or
players (an axe does 2.5). It's also quick on leaves. Make it from 0.5 planks and 2
stone.

**Armour** is made from deer **hide** (each deer you kill gives one, along with its
venison). There are three pieces, each made once and then always worn, like tools are
kept:

| Piece | From | Takes off |
| --- | --- | --- |
| Hide cap | 2 hide | 12% |
| Hide tunic | 4 hide | 24% |
| Hide boots | 2 hide | 12% |

All three take 48% off the damage from hits (creatures and other players) and traps,
rounded, but a hit always does at least half a heart. Armour doesn't help against
drowning, thin or stale air, or starving. The inventory shows your pieces beside the
tools (shaded until made) and the total. Armour is saved with you; operators can hand
it out with `/give cap|tunic|boots [player]`. The rules are in `src/world/Armour.h`.
`CraftTest` checks making the sword and the armour, that each piece is made once, the
damage sums, and that they're kept after a restart; `CreatureTest` checks that armour
takes a sword hit from 3.5 hearts down to 2.

## Chests

A **chest** (crafted from 2 planks) stores things. Right-click one within 6 blocks
to open it (Shift-right-click builds against it instead). The window shows what's in
it above what you have: click a block or item to move a block's worth (or one item)
across, and Shift-click to move all of that kind. I or Esc closes it, and so does
walking away. Anyone can open any chest, and everyone with it open sees changes at
once. Breaking a chest gives what was in it to whoever broke it. Chests can't be
shaped, and they're fastest to break with an axe.

The server keeps what's in every chest and saves it with the world (world format 8).
The client is only told what's in a chest while it has it open. `ChestTest` checks:

- crafting and placing a chest;
- opening it within reach and not from afar, and nothing going in unopened;
- moving things in and out, and not more than you have;
- a second player seeing the same contents and taking from them;
- breaking it (it closes, and the contents go to the breaker);
- the chest and its contents after a restart.

## Beds

A **bed** (3 planks and a hide) is half a block high. Right-click one within 6 blocks
to make it where you respawn: when you die you come back on it (or standing beside
it, if something's over it). If the bed has been broken, or is boxed in, you're back
at the world's spawn point and told why. Your bed is saved with you.

At night (the sun below the horizon) right-clicking a bed also puts you to sleep. On
your own, that skips to the morning. With others online, everyone has to get into a
bed within a few seconds of each other; the chat says how many are asleep. By day it
just says you can only sleep at night. `BedTest` checks:

- making and placing a bed;
- setting the respawn point, and not sleeping by day;
- sleeping through the night alone, and with two players only once both are in bed;
- respawning at the bed, also after a restart;
- going back to the spawn point once the bed is gone.

## Food and hunger

In survival you also get hungry. Ten drumsticks sit above the right end of the hotbar,
two points of food each. Food goes down by a point every 50 seconds, another for every
60 blocks you walk, and half a point for each half heart you heal. Hearts only come
back while you have more than 14 points (seven drumsticks). With no food at all you
starve: half a heart every 4 seconds, down to your last heart (starving alone won't
kill you). The drumsticks shiver when you're getting low. You respawn full.

To eat, put food in the hotbar and right-click with it, looking anywhere:

| Food | Gives | From |
| --- | --- | --- |
| Raw venison | 3 | killing a deer (one or two each, and a hide) |
| Cooked venison | 8 | crafting: 1 raw venison + 0.25 log |

Food is an item: it lives in the inventory and the hotbar like blocks do, but can't be
placed. You can't eat when you're full. Creative players don't get hungry. Your food
and health are saved with you. Operators can set someone's food with
`/food <0-20> [player]`, and anyone can check theirs with `/food`. The rules are in
`src/world/Hunger.h` and the server side in `Server::UpdateHunger` and
`Server::HandleEat`. `FoodTest` checks:

- getting hungry, and starving down to the last heart but no further;
- eating raw and cooked venison, not eating what you don't have or when full;
- healing only while well fed;
- venison from deer, and cooking it;
- that food can't be placed;
- that creative players don't get hungry.

## Crafting

In survival, the inventory (I) has a crafting list below your blocks. Each recipe
shows what it takes, and is greyed out until you have it. Make makes one, and x4
makes four.

| Makes | From |
| --- | --- |
| 4 planks | 1 log |
| Stone bricks | 1 stone |
| Glass | 1 sand |
| Mossy bricks | 1 stone brick + 1 leaves |
| Lamp | 1 glass + 0.5 planks |
| 2 pipes | 1 stone |
| Pump | 2 stone + 1 glass |
| Sluice gate | 1 stone brick + 1 planks |
| Pressure plate | 0.25 stone (pebbles will do) |
| Spikes | 1 stone |
| Pickaxe | 1 planks + 1.5 stone |
| Axe | 1 planks + 1 stone |
| Shovel | 1 planks + 0.5 stone |
| Cooked venison | 1 raw venison + 0.25 log |
| Sword | 0.5 planks + 2 stone |
| Chest | 2 planks |
| Bed | 3 planks + 1 hide |
| Hide cap / tunic / boots | 2 / 4 / 2 hide |

Amounts are in blocks, using the same resource units as building (a block is 64).
Tools are made once and kept, and saved with the player. In survival you can only
use the tools you've made: an unmade one in the hotbar is shaded over and works like
a bare hand, for mining and for fighting. The server checks that too. Creative
players have every tool and don't need to craft. Operators can hand out tools with
`/give pickaxe [player]`.

Two blocks are new with crafting:

- **Planks** are a building block (stepped when carved), fastest with an axe.
- **Glass** is solid, but you can see through it and light passes. Faces behind it
  aren't hidden, and touching panes join without a face between them.

The recipes are in `src/world/Crafting.h` and the server's checks in
`Server::HandleCraft`. `CraftTest` checks:

- the recipes;
- crafting several at once;
- that crafts you can't afford change nothing;
- that a tool is made once and kept;
- that creative doesn't craft;
- that tools survive a restart.

## Breathing

In survival you also need air. Ten bubbles appear above the right end of the hotbar
whenever something is taking your breath, along with what it is:

| Where | Your air | Once it's gone |
| --- | --- | --- |
| Head under water | drains, in 15 seconds | drowning: a heart a second |
| Thin air, above height 300 (far above the highest peaks: build up to reach it) | settles at a level that falls with height: half at 350, none from 400 | half a heart every 1.5 s |
| Stale air, below -100 (well under the dungeons) | the same going down: half at -150, none from -200 | half a heart every 1.5 s |

Thin and stale air don't drain your air over time: they only limit how much you can
hold, so you can stay at any height where there's some. Under water in thin air,
you start from that lower level. Anywhere else your breath comes back in five seconds. As your air runs low the edges
of the screen darken (blue under water), pulsing once it's gone. With half your air
gone in thin or stale air you're short of breath and can't sprint. Everyone hears if
you drown or suffocate, and you respawn with full air. In creative or no-clip nothing
takes your breath.

The server keeps the air meter, using the rules in `src/world/Breathing.h`.
`BreathTest` checks those rules. It also uses a client that stays wherever it's put
to check:

- drowning, in a water-filled well;
- thin air settling at its level at 345 and running out at 400;
- fine air at -95 and stale air settling at -130;
- recovery on the ground;
- creative.

## Chat and commands

Press T (or Enter) to chat: Enter sends the line to everyone, Esc closes the box. New
lines show in the bottom left for ten seconds, then fade; while the box is open, the
last few stay. Join, leave and server messages are yellow, errors red, private
messages purple.

A line starting with `/` is a command (press `/` to start one). `/help` lists the
ones you're allowed to use:

| Command | Who | |
| --- | --- | --- |
| `/help`, `/list` | everyone | list the commands, who's online |
| `/me <action>` | everyone | say what you're doing |
| `/msg <player> <text>` | everyone | a private message (also `/tell`, `/w`) |
| `/spawn` | everyone | back to the spawn point |
| `/time`, `/weather`, `/ops` | everyone | show the time, the weather, the operators |
| `/time <day\|noon\|dusk\|night\|midnight\|dawn\|HH:MM>` | operators | set the time (sunrise is 06:00) |
| `/weather <clear\|rain\|auto>` | operators | set the weather |
| `/tp <x> <y> <z>`, `/tp <player>`, `/tp <player> <x> <y> <z>` | operators | teleport; `~` means relative (`/tp ~ ~10 ~`) |
| `/save` | operators | save the world now |
| `/kick <player> [reason]` | operators | remove a player |
| `/op <player>`, `/deop <player>` | operators | make or unmake an operator |
| `/noclip [player]` | operators | fly through blocks, on or off (Space / E up, Q down) |
| `/gamemode` | everyone | show your game mode |
| `/gamemode <creative\|survival> [player]` | operators | change a game mode (also `/gm`, `c`/`s`) |
| `/give <block> [count] [player]`, `/give <tool> [player]` | operators | give survival blocks (`count` in blocks, default 64) or a tool |
| `/summon <deer\|ghoul> [x y z]` | operators | bring a creature (default: a few blocks ahead; `~` relative) |
| `/creatures [clear]` | operators | count the creatures, or remove them all |
| `/pvp`, `/food` | everyone | whether players can hurt each other; how fed you are |
| `/pvp <on\|off>` | operators | let players hurt each other, or not |
| `/food <0-20> [player]` | operators | set how fed someone is |
| `/stop` | console | shut the server down |

A dedicated server (`--server` or `VoxelServer`) also takes commands typed into its
console, with or without the slash; `stop` shuts it down cleanly.

**Permissions.** Players can chat and use the harmless commands; operators can also
change the time and weather, teleport, save, kick, make other operators, use no-clip
(`/noclip`) and the laser (L). The server enforces it: it ignores a player's laser and
flying unless they're allowed. The server
console is always an operator. In singleplayer you are too: the game makes its local
player the server's owner, who is always an operator and can't be removed. On a
dedicated server, make the first operator from the console (`op Alice`) or with
`--op Alice` on the command line; operators can then use `/op` themselves. The list is
kept in `ops.txt` in the world folder, one name per line (case doesn't matter), which
you can also edit while the server is stopped.

Players are only known by the name they join with (`--name`), and nothing checks it,
so anyone who knows an operator's name can join as them. The permissions keep players
from changing things by accident or casually, not from someone set on impersonating an
operator, so only host for people you trust.

`ChatTest` checks chat, every command, permissions (including `ops.txt` surviving a
restart and the owner) and the console, with a server and two clients.

## Day, night and weather

The server runs the clock and the weather and sends both to every player 20 times a
second, so everyone sees the same sky. A day lasts 20 minutes: the sun rises in the
east, crosses the sky (lighting the terrain, the water's glint and players from its
current direction), and the sky turns warm around sunrise and sunset and dark at
night. The time of day is saved with the world.

Now and then it rains: clear spells last 10 to 25 minutes and showers 2 to 5, fading
in and out over ten seconds. Rain greys and dims the sky, falls as streaks around
the player (not under a roof or underwater), and tops up lakes and pools (see
[Water](#water)).

`--day-length SECONDS` and `--weather clear|rain|auto` change these, on `VoxelEngine`
(singleplayer or `--server`) and `VoxelServer`. `WeatherTest` checks thin water, that
rain fills a drained pool to the brim and no further while leaving full, covered and
dry-ground water alone, and that the clock runs, saves, resumes and can be skipped.

## Fog

Toward the edge of the render distance, everything fades into the sky behind it.
Terrain, water and other players all fade, from a little over half the distance
(105 blocks at the default 12 chunks) to the edge itself (192 blocks). The fog
colour is the skybox's colour in that direction, with the same time-of-day tint,
night dimming and rain grey. So the world's edge, and chunks still loading out
there, blend into the sky instead of ending in a hard line. The fog is measured
horizontally, matching how chunks are loaded. It's `ApplyFog` in `main.frag`,
`water.frag` and `player.frag`, set up by `Renderer::SetFogUniforms`.

## Lighting

Two light channels, each 0-15, are flood-filled on the server's world worker thread and
cross chunk borders:

- **Sky light** comes down from above at full strength and loses one level per
  block sideways, so overhangs, tunnels and caves get darker the further they
  are from open sky. The day/night setting scales it in the shader.
- **Block light** spreads from lamps (level 15), losing one level per block.
- Both lose two levels instead of one entering water.

Leaves and plants let light through, so the ground under trees stays lit.

Faces combine both with per-vertex ambient occlusion, all smoothed across vertices.
Sky light depends on which way a face points: full on faces turned up, half on walls,
and 40% on faces turned down, so the underside of an overhang or a tree canopy is
lit by the sky around it instead of going black.

## Block textures

Every PNG in `Resources/Textures/Blocks/` is packed into a texture atlas at
startup (and again when you press R). Textures are referenced by file name
without the extension; the face-to-texture mapping for each block type lives in
`kBlockTextureNames` in `src/world/Block.cpp`. Tiles should all be the same
square size (others are resampled to match the first). Missing textures show up
as a magenta checkerboard.

## Sound

The game has sound effects, through OpenAL. Sounds in the world come from where
they happen and fade with distance.

- **Footsteps**, yours and other players', sound like the ground: soft on grass and
  dirt, hard on stone and bricks, gritty on sand and snow, hollow on wood.
- **Mining** taps on the block as you work. Breaking crumbles, cracks, knocks or
  shatters, by material. Placing thuds, and picking up pebbles pops.
- What **other players** build and break is heard too (at most a few at once, so a
  collapse or the laser isn't a din). The server's block changes are compared with
  what was there.
- **Hurt** grunts, splashes into water, a **thump** when a creature is hit, a ghoul's
  groan now and then, a chime when crafting, and **rain** while it rains where you
  are. Under a roof it's quiet.

Every sound is made by `tools/make_sounds.py` from noise and tones, so there are no
recordings to license. Run it to remake `Resources/Sounds/` (16-bit mono WAV, with
variants as `name_1.wav`, `name_2.wav` and so on, picked at random at a slightly varied
pitch).

On macOS the game uses the system's OpenAL framework. Elsewhere it uses an
installed OpenAL, such as openal-soft. Without one, or without an audio device, it
plays silently. The volume is in the settings. `VOXEL_MUTE=1` silences it, which is
handy for unattended screenshots. The code is in `src/audio/Audio.cpp`.

`SoundTest` checks, without a speaker:

- that every sound the game plays is there and loads;
- that blocks sound like their material;
- that a player gets events for another player's edits, and not their own.

## Menus and settings

Run with no options, the game opens at a title screen:

- **Singleplayer**: the worlds in `saves/`, newest first, each with its terrain and
  mode. You can play one, delete one (it asks first, and it can't be undone), or
  make a new one with a name, terrain (normal or empty) and mode (survival or
  creative).
- **Multiplayer**: join a server by address (`host` or `host:port`). The last one is
  remembered.
- **Settings**: your name, render distance (2 to 12 chunks), field of view, mouse
  sensitivity, vsync and volume. They're kept in `settings.txt` in the working directory.

In a game, Esc opens the pause menu: back to the game, settings, save and quit to the
title screen (or disconnect), or quit. A singleplayer world keeps running while
paused, as a server does. If the server drops you, the menu says why and takes you
back to the title. Field of view, sensitivity and vsync change at once. A new render
distance and name apply from the next game. The render distance is sent to the
server when you join, and it streams only that far (never further than its own 12
chunks), so a short one really does save work.

For unattended screenshots there are a few environment variables:

- `VOXEL_MENU_PAGE=worlds|new|multiplayer|settings` opens that page;
- `VOXEL_PAUSE=1` opens the pause menu;
- `VOXEL_RUN="/time noon;/summon deer"` sends those chat lines a second after landing;
- `VOXEL_HOLD_MINE=1` mines whatever's in view.

These go with the existing `VOXEL_AUTO_SCREENSHOT`, `VOXEL_NO_MOUSE` and
`VOXEL_OPEN_INVENTORY`.

## Performance

Most loaded chunks are solid ground or empty air and have no mesh. The client only
draws chunks that can be seen: each frame it floods out from the camera's chunk
through the view frustum, passing through a chunk only between faces that its open
cells connect (worked out when the chunk is meshed), and never back toward the
camera. So caves under solid ground aren't drawn from the surface. A typical view
draws about 500 chunks out of 15,000 loaded (about 2,750 by frustum alone; Apple's
OpenGL is slow per draw call, so this roughly doubled the frame rate). Chunks are drawn
nearest first. The water pass (including its full-screen copy for refraction) is
skipped when no water is in view.

`VOXEL_BENCH=1 ./VoxelEngine` turns vsync off and prints, once a second, the frame
rate, where the frame time goes (network, update, draw submission, GPU, UI) and how
many chunks were drawn.

## Layout

```
src/
  main.cpp  command line: singleplayer, --server, --connect, --world
  app/      Application (window, input, main loop), Menu (title screen, pause menu),
            Settings, Camera, Player (movement and physics)
  client/   Client (connection), ClientWorld (received chunks, mesh worker),
            ClientChunk (meshes and GL buffers), ChunkMesher
  server/   Server (clients, streaming, validation), ServerCommands (chat and
            commands), Creatures (spawning, AI, fights), ServerWorld (chunk loading,
            generation and light worker), Water, WorldStorage (saving),
            main.cpp (VoxelServer)
  net/      Protocol (messages, RLE chunk planes), Transport (ENet wrapper)
  audio/    Audio (OpenAL sound effects)
  render/   Renderer (frame passes, skybox, post effect), Rain, PlayerModels, Shader,
            Textures, DebugDraw
  world/    Shared by both sides: Chunk, Block, BlockAccess, TerrainGenerator, Caves,
            Features, Dungeons, Noise, Lighting, Collision, Raycast, Mining,
            Breathing, Creatures, Crafting, Hunger, Armour
tests/      NetSmokeTest, SaveTest, WaterAreaTest, PumpTest, EvaporationTest, WeatherTest,
            ChatTest, DungeonTest, TrapTest, ShapeTest, BiomeTest, ModeTest, MiningTest,
            BreathTest, CreatureTest, CraftTest, SoundTest, FoodTest, ChestTest, BedTest
tools/      make_sounds.py (sound effects), make_item_textures.py (item icons)
```
