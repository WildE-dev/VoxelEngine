# VoxelEngine

A small OpenGL voxel engine with sloped terrain, caves, trees, chunk streaming on a
background thread, block lighting, ambient occlusion, finite flowing water and a
skybox. Deer roam it, and dwarves hold camps and mines. Several players can share a
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

On Windows, use the `windows` preset (Ninja and MSVC) from a Visual Studio
Developer PowerShell, or open the folder in Visual Studio, which picks up the presets:

```powershell
cmake --preset windows
cmake --build build-windows
cd build-windows; .\VoxelEngine.exe
```

(Windows has no system OpenAL, so the game is silent there unless openal-soft is
installed.)

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
[Chests](#chests)), `BedTest` (see [Beds](#beds)), `PowerTest` (see
[Air power](#air-power)), `FactoryTest` (see [Air power](#air-power)), `SignalTest`
(see [Signals](#signals)), `SyncTest` (two players: structures, chests, fluids, miners, the king and hired guards look the same to both), `ModelEditor` (see [The model editor](#the-model-editor)), `DataTest` (see [Editing blocks and creatures](#editing-blocks-and-creatures)) and `SoundTest` (see
[Sound](#sound)).

`ctest` in the build directory runs them all, and `ctest -j16` runs them all at
once (about half a minute on 16 cores). The longest ones (CreatureTest, FactoryTest,
BreathTest, TrapTest and FoodTest) run in parts side by side: each group of their
sections is a ctest test of its own, such as `CreatureTest.raids`, run as
`CreatureTest raids` (see `tests/TestSections.h`; the groups are listed in
`CMakeLists.txt`). Run with no group, a test runs everything. They're written to pass
even all at once, on a loaded machine:

- They wait for things to happen rather than for fixed times.
- Where the client shows a change before the server makes it (building, for
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
  times a second. The server checks it:
  - **Speed:** across the ground, no faster than the mode allows. That's 11 blocks a
    second in survival (a sprinting boat does 9) and 24 flying in creative, more for a
    moment and a half after a hit or a blast knocks you. Falling isn't limited, short
    of 55 a second.
  - **Air:** in survival, no higher than a jump above where you last stood (in water or
    lava, on a ladder, in a boat), and not staying up for more than two seconds
    without coming back down.
  - **Solid blocks:** not inside them for long, chiselled quarters and turned shapes
    included.

  A move that fails puts the player back: to the last good position, or for flying, to
  where they last stood. No-clip players aren't checked. `MiningTest` checks that
  running too fast, flying up and hovering are put back, an ordinary step isn't, and a
  creative player may fly. Others are shown 100 ms in the past, interpolated between
  snapshots.
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
  game modes and inventories) and chests; `regions/r.X.Y.Z.vxr` holds the stored chunks of an 8x8x8-chunk
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

These are the default keys. **Settings > Controls** rebinds them: click an action, then
press its new key (Esc keeps the old one); a key bound to two actions shows in red, and
"Reset to the defaults" puts them back. They're kept in `settings.txt` (`key_jump 32`
and so on). The number keys, Esc, F1-F4, F12 and the mouse buttons stay fixed.

**Gamepads** (any GLFW knows) work too: the left stick walks and the right stick looks
(its speed is in the settings), A jumps, B crouches, pressing the left stick sprints,
the right trigger mines and hits (hold it to keep mining), the left trigger places and
uses, the bumpers change hotbar slot, Y opens the inventory, Back the map, and Start
pauses. The menus can be driven with the pad as well.

| Key | Action |
| --- | --- |
| WASD | Walk (or fly) |
| Space | Jump (swim: up; fly: up) |
| Shift | Sprint (fly: faster); the view widens a little while you sprint |
| Left Ctrl (hold) | Crouch: lower (fits under 1.5-high gaps) and slow, and you won't walk off edges; your name tag hides. You stand again once there's room. (Flying: down) |
| Q / E | In no-clip (`/noclip`, operators): fly down / up |
| Mouse | Look (a crosshair marks the middle of the screen) |
| Left click | Break block (survival: hold to mine it), or hit the creature in front of you |
| Right click | Place the selected block; eat food in hand; open a chest, a machine's menu or a structure's, whatever's in hand; use a bed (crouching: place against it instead). With a block in hand, pipes, shafts, wheels and valves are built onto |
| Middle click | Pick the block you're looking at: creative puts a stack of it in the selected hotbar slot; survival selects the hotbar slot holding it, or swaps a stack of it from the rest of the inventory into the selected slot |
| V / Crouch+V | Placement height for building blocks: full, 3/4, 1/2, 1/4 (and back); with the chisel in hand, how fine it cuts: 1/8, 1/4, 1/2 |
| 1-9, 0 / mouse wheel | Select the hotbar slot to place from |
| Tab | Inventory: move stacks between slots, the hotbar and your armour; craft (survival), or take any block or item (creative). Let go of a stack outside the window to throw it away |
| G / Crouch+G | Throw one of the item in hand / the whole stack: tap to toss it, hold to throw it further (a bar under the crosshair fills as you wind up) |
| L (hold) | Laser, operators only: carve a tunnel along the view ray |
| R | Reload shaders, block textures and the data files (blocks, creature models), rebuild all chunk meshes |
| F1 | Toggle wireframe |
| F2 | Toggle debug window |
| F3 | Toggle ambient occlusion |
| F4 | Toggle the compass, coordinates and clock at the top of the screen |
| P | Toggle the list of players online, with how far off each is and which way |
| M | Toggle the minimap (top right): the ground around you from above, north up, with you as an arrow and other players as dots |
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

Ranges rise out of the hills over about a third of the world: a very large-scale
noise field decides where they run, and ridges inside them (rounded at the crests,
with smaller ridges on their flanks) climb up to about 150 blocks above the hills, so
peaks reach around 160. Above row 44 the ground is bare rock, with nothing growing
(trees, bushes, grass and flowers stay below that tree line); above row 70 it's snow.
It gets colder the higher you go (a little colder every block above row 12), so high
ground turns to taiga and tundra. The slopes are kept mostly under 45 degrees, since
each column's surface is one sloped block (a few mountain columns are steeper, and
show a step). The nearest ground above row 80 is about 750 blocks from spawn, towards
+x +z.

### Lakes

The hills and mountains hold lakes: the world is divided into 128 x 128 cells, and in
most cells above the lowlands a round lake 14 to 34 blocks in radius sits in a bowl
carved into the land, up to 8 blocks deep, with a sandy bed. Its level is set near
the lowest ground around it, and its rim is raised just above the water all the way
round (on the uphill side the shore eases into the slope instead), so a lake is
always contained, however the land falls away below it. They're high above the sea
and full of still water: tap one and it drains downhill, which is what water wheels
want (see [Air power](#air-power)). Mountain lakes never freeze, no trees grow in or
right beside them, and caves never open under them. About one cell in six has one;
the nearest to spawn is about 70 blocks out, towards -x -z.

**Rivers.** Where a lake's rim is lowest and the land beyond falls away below its
level, it overflows: a **spring** sits in the ground just outside the rim, and water
wells up out of it without end, a block and a half a second (see [Water](#water)).
From it a river runs downhill, up to 160 blocks: a channel about three blocks wide
cut two blocks into the land, following its fall and turning gently, its bed never
rising (it cuts through small rises), with banks easing up to the ground either side.
The bed goes down in level steps, a pool and a drop at a time: it stays level until the
land comes down to it, then drops a block where the land falls gently, or three where
it's steep (a small waterfall into a pool cut back into the slope; a mountain river is
a chain of them). Just before each drop a **weir** of sand slabs half a block high runs
across the channel, and each pool is generated half a block deep, full to the weir's
top and still: the spring's water tips each one over into the next all the way down,
so the river keeps flowing (water deeper than its weirs would just stand still) and
lies flat in its pools instead of spreading in a thin film down a slope.
Its bed is sand over dirt, and no trees grow in it or on its banks. It ends where it reaches the sea, or where it would
have to cut more than seven blocks deep, at a **sinkhole**: a dark opening in wet
rock in its bed (a natural drain) that takes water away twice as fast as the spring
gives it, so the river keeps
running from spring to drain without flooding the land beyond. Rivers make ready-made
mill sites: a water wheel with one side in one turns for as long as the spring runs.
Springs look like wet rock and sinkholes like a dark hole in it; breaking either
gives stone and stops it. For your own builds, a **drain** (an iron grate, crafted from
half an iron ingot and a stone) takes water like a sinkhole, but sends it into fluid
pipes instead of destroying it (see [Oil and fluids](#oil-and-fluids)). `/locate spring` and `/locate sinkhole` (operators) say where the nearest is.

### The Deep

Far down, from 100 to 160 blocks below sea level, lies **the Deep**: a band of huge
caverns, dark but for **glow fungus** growing on their floors (it gives light 11, and
can be picked and planted elsewhere) and pools of **magma** (light 12; standing on it
burns, half a heart every 0.75 s). **Rock grubs** live there: low, six-legged crawlers with stone shells
and glowing spots that come out of the dark (light 6 or less) around a player in the
Deep, up to six at once, and go for them (a heart a bite; 5 hearts to kill). Its rock
is richer in iron, and it's the only place with **gold ore**, which smelts to gold ingots: what dwarf traders deal in (see
[Markets](#markets)). `BiomeTest` checks the Deep's caverns, fungus, magma and gold, and
that there's no gold above it.

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
| Pump room | a pump in a sunken basin with a fluid pipe up into a raised tub (it stands idle until someone brings it air) |
| Shrine | a lamp on a plinth, pews facing it |
| Flood chamber | a pressure plate in the floor sets a latch gate in the ceiling, which opens the sluice gates beside it and keeps them open: the reservoir above pours in, and goes on pouring after you step off. A lever hidden in a wall by a corner resets the latch (and, left on, disarms the trap) |
| Lever vault | four levers in a row by one wall, with a lamp in the wall over some of them; set the ones under lamps on and the rest off, and a gate in the middle of the floor opens over a pit with a chest of vault loot in it. Logic gates in the ceiling do the sums (NOT for the levers that should be off, AND for all four), and the gate closes again if you change them |

**Traps.** Some long corridors have a spike pit across them: either hidden under
crumbling bricks (three long; they're cracked, if you look) or open (two long, so you
can jump it). A vault's lit pedestal is ringed by crumbling floor over spikes. Fall in
and there's no climbing out. Flood chambers flood when you step on their plate, and
keep flooding until someone finds the lever. Lever vaults are puzzles, not traps:
the lamps say which levers to set.

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
straight through a room), water sits in sealed basins, and a pump room's pump is
ready to work once someone runs an air pipe to it (see [Air power](#air-power)). It costs little: chunks still generate in about
0.15 ms. New blocks: stone bricks and mossy bricks (on the hotbar after 0; pick them
with the mouse wheel). Empty worlds (`--empty-world`) have no dungeons.

`DungeonTest` lays out 225 regions and generates 16 dungeons chunk by chunk. It checks
that every room type turns up, layouts and chunks are deterministic, every room can
be reached from the entrance across chunk borders, shafts reach the surface, water
can't spill, rooms stay deep, gardens and archives are furnished all over regardless
of chunk borders, pump rooms are wired up, and vault and archive chests are there with
loot in them (and none for a chest outside a dungeon). For lever vaults, it checks
the levers, floor gate and chest are there, works the wiring through for all 16 ways
the levers can be set (the gate opens for the room's pattern and no other), and
checks the lamps match the pattern.

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

**Pumps and fluid pipes** move water anywhere, including uphill, but
pumps run on compressed air (see [Air power](#air-power)): without air pressure a
pump stands still. Each tick a powered pump takes up to 5/8 of a block of water (2.5
blocks a second) from the cells touching it and sends it through the fluid pipes joined to it, face to face, out of the
network's open ends. An open end is a pipe joined to just one thing, and its water comes
out of its end face only (the side away from what it's joined to), into the cell there.
An end against a block is closed. The ends share the water equally, and the pump only takes what they
have room for, so water is still never created or lost. If the pipes also reach a fluid
tank or a fluid machine (an outlet, a boiler, a refinery...), that fluid network takes
an equal share, as far as it has room. Open ends still spill, whatever the other ends
are joined to. A pump whose pipes hold another fluid (oil, fuel, steam) stands: they
aren't for its water. A pump draws only from the
cells it touches, so put it at the bottom of a pool to empty it. Pumps run within
the water simulation distance, like flowing water, and keep running after a save
and reload. Fluid pipes are the one pipe for liquids: the same pipe carries a pump's
water and the fluids of [Oil and fluids](#oil-and-fluids). The old stone water pipe is
gone: any left in a saved world, an inventory or a chest becomes a fluid pipe. Pipes are
drawn as thin tubes, with an arm out to each pipe, pump, machine or gauge they join, and
you bump into the tube, not the whole cell. `PumpTest` checks that a pump without
air moves nothing and a little air lifts a little, the rate, that water is conserved,
that pumps stop when the pipe is cut or the outlet is full, and that saved pumps
carry on.

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

**Springs** add water: each one puts up to 48 units a tick (three eighths of a block)
into the open cells above and beside it that have room, shared between them, for as
long as it's within the simulation area. Sinkholes take it away: up to 96 units a
tick from the cells above and beside them. A player's drain takes the same way but
only as much as its fluid pipes have room for, into them; a fluid outlet lets the
pipes' water out (see [Oil and fluids](#oil-and-fluids)). Water that reaches the open sea goes: random
ticks take any water lying above the sea's surface on the open sea, so streams don't
slowly raise it. `PumpTest` checks that a spring fills the cell above it at that rate
and stops when it's full, keeps the water coming once there's somewhere for it to
go, and stops when broken, and that a sinkhole empties a cup at its rate; `BiomeTest`
that lakes overflow from springs, each in the ground, open above and below a lake's
level, that rivers end in sinkholes in their beds, and that their pools have sand
beds with water over them.

**Evaporation**: shallow puddles that have stopped flowing dry up. Every water tick,
8 random cells of each chunk near a player are looked at (random ticks), and a resting
puddle among them disappears: a cell with at most a quarter of a block of water that
isn't full, has no water above it and isn't flowing. Each cell comes up about every
2.1 minutes on average, so a spill fades away cell by cell. This, drains and the sea
are the only ways water is ever destroyed (and springs the only way it's made); lakes and pools are deeper than that and stay. Random ticks
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

## Lava and steam

**Lava** (creative only, in the inventory next to water; it also lies in pools on the
Deep's cavern floors) is a glowing liquid that flows like water, only slower and
thicker. A cell holds up to a block's worth, 128 units, and it's only ever moved, never
made or lost. Every half second (a tenth of water's pace) it falls, as much as fits,
into open space or thinner lava below. With nowhere to fall, it levels out with the
cells beside it. It doesn't spread thinner than about a quarter of a block, so a block
of lava ends as a small pool of a few cells. The Deep's pools lie at rest until
something beside them changes, so loading the Deep costs nothing. It lights its surroundings as brightly as a lamp. You
can wade in it slowly (like swimming, at half the speed), and it burns for a heart
and a half every half second. It can't be aimed at or broken, but a block put where
it is replaces it.

**Water touching lava** boils away at once into a big cloud of steam, with a hiss.
That happens whether the water flows up to the lava or the lava runs into the water,
and the lava stays. A block of water makes 5,120 units of steam. Ordinary
evaporation doesn't make steam: puddles just dry up.

**Steam** is a gas in the open air, kept by the server cell by cell (not in the
blocks). Four times a second each cell shares its steam with the open cells around it
that have less. Most goes up (thick steam rises hard, thin steam hardly at all), some
goes to each side, and a little goes down. So a burst billows up, spreads out under
anything above it and hangs there, thinning as it grows. It also fades by about 2% a
step, so a block's worth of steam is a cloud of a hundred or so cells that's gone in
about twenty seconds. It never goes into solid blocks, water or lava, and a block put
where it is pushes it up. The server sends the steam within 64 blocks of each player
five times a second, and the game draws it as a translucent cloud filling the cells
it's in. Each face between a cell of steam and its neighbours is a see-through square,
as misty as the steam beside it is thick, so looking through more steam looks through
more of them: a thick cloud is near white and its thin edges barely there. Each face
takes its mist from its corners, each the mean of the eight cells round it, so it
fades smoothly across the cells and to nothing at the cloud's edge, blending them into
one cloud rather than a stack of boxes. Drifting noise in the shader breaks it up, so
it reads as cloud, not glass. With your head in steam, the view whites out, more the
thicker it is.

**Thick steam scalds.** A player with 200 units or more of steam in a cell their body is
in loses half a heart every 0.75 seconds (armour doesn't help), so stand back from
lava when you pour water on it. Thin steam at a cloud's edge is harmless.

**Obsidian.** Water touching lava always boils into steam. Half a block of water or more
at once also cools the lava it touches into **obsidian**, while a trickle leaves the lava
as it is. So pour a bucket's worth (a pump, a sluice) onto a lava pool to make it.
Obsidian is very hard: 30 seconds by hand, 7.5 with a stone pickaxe, faster with iron
or steel. Nothing else breaks it: TNT's blast leaves it standing, and raiders can't hack
through it. It makes **obsidian pipes** (four from one), the only pipe that holds lava. An
obsidian pipe touching lava draws it in, 32 units a second, as far as its network has
room; any other part in the network (a fluid pipe, a tank) and it won't take lava.
Obsidian pipes from a lava pool to the side of a **boiler** fire it instead of coal,
burning 4 units of lava a second.

`SteamTest` covers both rules on their own:
- steam rises and spreads, stays under a roof, keeps out of water and fades away;
- lava spreads into a small pool keeping every unit, fills a pit from the bottom, and pours over an edge;
- water beside lava boils into steam.

It also runs the whole path over a real connection: a creative player pours water
next to their lava, the steam reaches the game and fades, and a survival player can't
place lava.

## Fire

Fire is a real danger. It sits in an open cell beside something that burns, and four
times a second it:

- **Eats what it touches.** Each burnable block beside it goes sooner or later, and the
  fire often takes its place. Leaves and plants go in a moment; planks, doors, ladders,
  signs, beds and chests in a few seconds (a chest's contents burn with it); logs slowly
  but surely.
- **Spreads.** It tries the cells round it, higher ones most (fire climbs), and catches
  in an open cell beside something that burns, as readily as that burns. A wood goes up
  in under a minute. Rain on an open fire slows it and now and then puts it out, but it
  won't stop a blaze.
- **Dies down** a few seconds after it has nothing left to burn.
- **Burns.** Step in it and you catch fire, losing half a heart every half second, and
  you go on burning for four seconds after you're out of it, unless you get into water.
  Creatures in it are hurt, items lying in it are lost, and TNT beside it goes off.

Lava sets fire to what burns beside it, lightning to where it strikes, and a TNT blast
to what's round it. Players can't light fires (creative can place it, for testing).

It's hard to put out. You can't punch it out, since aiming passes through flames.

**Fighting it:**

- **A bucket** (three quarters of an iron ingot). Right-click a full block of water to
  scoop it up, then right-click to pour it out where you're looking. The splash puts out
  every fire round where it lands, and puts you out if you're burning.
- **Water** flowing into a fire puts it out: a pump, pipes and an outlet make a hose, or
  open a sluice.
- **Smothering.** Put any block that doesn't burn (sand, dirt, stone) where a fire is.
- **A firebreak.** Clear or chop what burns in its path, and it dies down at the gap.
- **A sprinkler** (a fluid pipe and a copper ingot). Fix it under a ceiling, joined to
  fluid pipes or a tank of water. While it sprays it puts out every fire within 4
  blocks across and from 6 below it to 1 above, and puts out anyone burning there. Each
  second of spraying uses 16 units of water. With no channel, it sprays by itself
  whenever there's fire in that area. Give it a channel (see [Signals](#signals)) and it
  sprays only while that channel is on.
- **A heat sensor** (half an iron ingot, half a copper ingot and half a glass) sends on
  its channel while fire or lava is within its range (4 blocks unless set, up to 8). Put
  one where a fire would start and wire it to sprinklers (or a sluice gate, or a lamp
  as an alarm) further off.

While you're on fire, flames lick up round the edges of your screen with a crackle,
and other players see flames up your body.

Flames are drawn as glowing licks of fire shooting up from each burning cell, with
smoke, a crackle, and the light of a lamp. `FireTest` checks that a fire spreads
through a little wood and burns it down, dies out on bare stone, leaves stone and
obsidian alone, is put out by water, by smothering and by a bucket, burns a survival
player who keeps burning until in water (their client told so), and destroys items
thrown into it. It also checks that a sprinkler on a tank of water puts out the fires
below it by itself; that a sprinkler on a channel waits for it; and that a heat sensor
turns on near fire, sets the sprinkler off on its channel, and goes off once the fire
is out.

## Player

You walk on the terrain with gravity and collision. The body is a 0.6 x 1.8
box. Full blocks collide as boxes, and chiselled quarters and turned shapes as
exactly what they fill, so you walk right up against a raised quarter. Sloped
terrain blocks collide as a height field that matches their rendered triangles:
you stand on the highest point of the visible surface under you and walk up and
down hills smoothly. A slope higher than a step above your feet is ground over you
instead: jumping or flying up into it, your head stops at its underside, and you
can't walk into it at head height. Creative flight treats slopes as solid all round. Ice has hardly any grip, so speeding up, stopping and
turning on it all slide. You can step up ledges up to 0.6
blocks; anything taller needs a jump (about 1.25 blocks high). Physics runs at
a fixed 120 Hz. Until the chunks under you have loaded, you wait in place. Fly
mode (F) moves freely through blocks like the old free camera.

## Block shapes

A block can be shaped by its four quarters, each 0 to 8 eighths high (the terrain's
sloped tops are shaped this way). How a shape is drawn, and stood on, depends on the
block: natural ground (grass, dirt, sand, stone, snow) slopes smoothly between its
four corners, while anything built (bricks, logs, lamps and the rest) is four
quarter-columns, each as high as its corner, like small steps.

Shape blocks yourself with the chisel, a tool (in survival, made from a little wood
and stone). With it in hand, right-click lowers the quarter you're looking
at, and crouching and right-clicking raises it again. **V** sets how far each stroke
goes: an eighth of a block (as fine as natural slopes), a quarter, or a half. Raising every quarter to the top
makes the block whole; lowering them all removes it.
The quarter you're looking at is the one your view actually lands on (a low step
seen past a taller one, the side of a quarter, the underside of one chiselled from
below), and while the chisel's in hand it's outlined in gold, so you can see which
one a stroke will take before you make it.

Chiselling a whole block on a side, or underneath, turns it that way first: that side
becomes its top, and the shape is cut down from there. A log keeps its grain running
the way it did. Turned shapes are drawn, shaded (corner shadows and all), aimed at
and collided with as what they are, and hold water above what they fill of each
quarter. Building one back up whole makes it an ordinary block again.
 Machines (pumps, pipes, sluice
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
and fold, a stepped block's quarters, a pressure plate's slab, each of a pebbles
block's stones, the part of a plant's picture with anything in it), and blocks in
front of it hide it. Aiming follows shapes too: looking over a slope or past a
quarter-column targets the block behind it, so you can break and build through the
open part of a partial block. Grass, flowers, saplings and crops are aimed at by the
solid pixels of their pictures, and pebbles by their stones, so you can click
through the gaps to the ground behind.

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

Everything you carry is in a grid of **slots**: the ten of the hotbar (keys 1-9 and
0, or the mouse wheel) and thirty more. Each slot holds a **stack** of one kind of
thing: up to 64 of a block or item, or a single tool or piece of armour. What's in the
selected hotbar slot is what's in your hand: a block to build with, food to eat, a
tool to work with. An empty slot is a bare hand.

Press I to open the inventory: your armour, the thirty slots, and the hotbar below
them. Click a stack to pick it up (it follows the cursor) and click a slot to put it
down, or drag it there. Onto the same kind it adds what fits; onto something else it
swaps. Right-click picks up half a stack, or, holding one, puts down one at a time.
Crouch-click moves a stack across: between the hotbar and the rest, into an open
chest and back, and armour on and off. Clicking off the slots lets go. I or Esc
closes it. The server makes every move, so a modified client can't make things up.

**Armour** has six slots of its own, one for each piece (cap, tunic, boots, shield,
steel plate, oxygen tank), and a slot takes only its own piece. What's in them is what
you wear (see [Swords and armour](#swords-and-armour)).

**Tools are items** too. A tool comes in three tiers, **stone**, **iron** and
**steel** (marked Fe and St on its icon), and a tool works at the tier of the best one
of its kind you carry (see [Ores and iron](#ores-and-iron)). Without one, it's a bare
hand.

In **creative** the inventory also shows every block and item, every tool in each
tier, and every piece of armour: click one to take a full stack (right-click: one),
and put a stack back there to get rid of it. Crouch-click puts a stack in the selected
hotbar slot. Nothing you build with runs out. A creative player who joins with an
empty hotbar gets the usual building blocks in it (or what an older `hotbar.txt`
chose).

The server keeps everyone's slots, armour and offcuts (see [Game modes](#game-modes))
and saves them with the player. Worlds saved before inventories (world format 13 and
older) load with what each player had turned into stacks: every block type's units as
whole blocks plus an offcut, each tool they had made at the tier of their upgrade, and
their armour in its slots. Old chests become stacks the same way. Nothing is lost: a
player or chest with more than fits gets extra slots, which go again once emptied.
`SaveTest` checks an old save converts, and saves and loads again.

## Game modes

There are two, and each player is in one:

- **Survival** (the default): you build with what you collect. Breaking a block
  drops its worth in that block type, and placing one costs it. Traps hurt, and
  you have hearts. You can break and build up to 6 blocks away.
- **Creative**: every block, as many as you like, and nothing hurts you. You can
  break and build up to 50 blocks away. **Double-tap jump to fly** (and again to stop):
  jump goes up, crouch or Q down, sprint flies twice as fast. Unlike no-clip it's solid
  (blocks stop you), and coming down onto the ground lands you.

The server checks the reach too (with a little slack for movement).

**Items on the ground.** What a broken block gives pops out of it as an item: a
little turning cube for a block, a card of its icon for anything else. If you broke
it and are within 8 blocks with room for it, it flies to you. Otherwise it lies
there until someone walks over it (within about a block and a half, it drifts in).
A broken chest spills what was in it. G throws one of what's in your hand (Crouch+G
the stack): a tap tosses it a few blocks, and holding G winds up a harder throw, up to
about three times as far after a second (a bar under the crosshair shows how hard), and in the inventory you can let go of a stack outside the window to
throw it. What you throw lands a few blocks ahead, where you can't pick it up for
a moment, and it doesn't come back on its own. What won't fit in a full inventory
(a kill's spoils, a catch) drops at your feet instead of being lost. Items fall,
land on blocks and slopes, slide (far on ice), float up in water, rise out of a
block placed over them, and join up when the same items lie together. They're gone
after five minutes, and are saved with the world.

A full block is worth 64 units. Shaped blocks are worth their volume: a half slab is
32, a quarter slab 16, and a chiselled block whatever's left of it (a slope its mean
height). Slots only hold whole blocks, so each block type also has an **offcut**: what
you have of it short of a whole block. Placing a quarter slab takes a quarter from the
stone offcut, and if that's short, one whole stone is cut into it first (leaving three
quarters). Breaking or chiselling a shaped block puts its volume back in the offcut,
and every whole block's worth goes back into a slot. Recipes that take part of a block
(half a plank, a quarter of coal) work the same way. Offcuts stay where they are: only
whole blocks and items move between slots and chests. The inventory lists your
offcuts under the slots. Press V to pick the height you place building blocks at (full, 3/4, 1/2,
1/4; Crouch+V goes back up). The hotbar shows the selected item's name and, for building
blocks, the height they're placed at; it's reddish when you haven't enough of a block,
and shows how many are in each slot. Chiselling a block down gives back what it takes
off, and building it up costs the same.
Plants, pumps, pipes, sluices and pressure plates can't be shaped, so they're
always worth a whole block. Pebbles are the exception. They're small clusters of
stones lying about on the ground, more often on beaches and bare rock, and they're
picked up at once with a click. Each one counts as a quarter of a block of stone
(16 units), and putting pebbles down costs the same. Water can't be collected, so in survival it can't be
placed either; use pumps to move it. The laser gives nothing.

The server keeps everyone's mode and inventory and saves them with the player. It
refuses an edit you can't afford, so a modified client gets nothing for free. With a
full inventory, a block whose worth won't fit won't break (the chat says so); nor will
a chest whose contents won't fit. Spoils that won't fit (venison from a hunt, a fish)
are lost, and the chat says that too.

A new world starts players in survival, or in creative with `--empty-world`, since
that world is for testing. Choose with `--mode creative|survival` (for `VoxelEngine`
and `VoxelServer`). The world remembers the mode it was made with. Operators change a
player's mode with `/gamemode <creative|survival> [player]`, and hand out blocks and
items with `/give <item> [count] [player]`, for example `/give stone_brick 32` (a
block's count is in blocks, 64 if left out, and may be part of one), `/give
iron_pickaxe` or `/give cap 2 Alex` (tools and armour: one if left out; armour goes on
if its slot is free).

`ModeTest` checks:

- what blocks are worth;
- that breaking gives and placing costs, at each height;
- that what you can't afford isn't placed;
- that chisel refunds and costs balance;
- that spikes hurt in survival and not in creative;
- that `/gamemode` and `/give` are for operators;
- that modes and what players carry survive a restart.

## Health and traps

In survival you have ten hearts, shown above the hotbar (creative has none). The server keeps track of them, so traps
work the same for everyone:

- **Spikes** cost two hearts every half second you stand in them.
- **Crumbling bricks** give way 0.4 seconds after you stand on them, and the collapse
  spreads to the crumbling bricks touching them, so a whole pit cover falls in.
- **Pressure plates** send on their channel while they're stood on (see [Signals](#signals)): a sluice gate listening to it opens.
- **Falls** of more than 3 blocks cost half a heart for each block beyond that (an
  11-block drop is four hearts). Landing in water, or on a **hay bale**, is safe. The server measures the
  fall from the highest point since you were last on the ground, deciding "on the
  ground" from the world rather than from what your client says. Teleports, respawns,
  flying and creative start the count afresh. Armour doesn't help. Running or bounding
  down a slope or a stair of steps isn't one long fall: whenever you're within 2.5
  blocks of the ground and not yet falling fast, the count starts again. And landing on
  a sloped surface hurts by how hard you hit it (coming down a mountainside with the
  slope barely hurts), not by how far you dropped.

The screen flashes red when you're hurt, and when you die it goes dark red for a few
seconds with "You died!" and how it happened. Hearts come back one every 4 seconds once
nothing has hurt you for 5, as long as you're well fed (see
[Food and hunger](#food-and-hunger)). With none left, everyone hears how you died and you
respawn at the spawn point (or your bed: see [Beds](#beds)) with full health (and three seconds' grace). In survival,
half of every kind of block and item you carry (rounded down) is left behind in a
chest placed where you died (or just beside or above, in the nearest open cell), and
the chat tells you where; anyone can open it, so a killer can take it. Your tools and
armour aren't dropped. In no-clip
nothing can hurt you. All four blocks are in the inventory, for building traps of
your own. `TrapTest` checks spikes, death and respawn, crumbling floors, pressure
plates and sluice gates, falls (safe from 3 blocks, four hearts from 11), and that
no-clip and the laser are operators' only.


Your bed shows on the minimap as a little red-roofed house (at its edge, if it's
further off). Where you last died, the minimap shows a red cross (at its edge, if it's further off,
the way to it) until you get back there: half of what you carried is waiting in a
chest on the spot.
## TNT

**TNT** (a sand and half a coal) explodes. Right-click it to light it: it turns white
and three seconds later blows a round crater three blocks out in every direction.
Everything in the blast goes (dungeon bricks and water too, and a chest's contents
are lost), and other TNT caught in it goes off a moment later, so a row of it sets
off a chain. Players within six blocks take up to five hearts, the closer the worse
(armour helps), and are thrown back; creatures are hurt too. Everyone hears the boom,
sees smoke and debris fly, and the view shakes if it was close. `TrapTest` checks
lighting it, the crater and its size, and the damage.

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
30%: a half slab takes half as long. The wrong tool is no better than a hand. In
survival you make the pickaxe, axe and shovel (see [Crafting](#crafting)); in creative
take them from the inventory. Put one in a hotbar slot to hold it. A tool places
nothing on a right click. In creative everything breaks at once, as before.

**Felling trees.** Breaking a log with an axe fells it: every log straight above it,
as far as the trunk goes, comes down too, each giving its worth. Leaves need a log
near them, within 4 blocks. When a log goes, leaves around it that no log holds up
any more fall away one by one over the next few seconds, now and then dropping a
sapling. Leaves you put down never fall: the server remembers them, with the world.

**Things knocked off drop.** A flower, crop, sapling, sign, ladder or other plant
whose ground goes, and a lever whose block goes, drop as an item where they were.
`MiningTest` checks that an axe fells a trunk, any other tool takes one log, a felled
tree's leaves fall away, leaves still held up and leaves a player put down stay, and
a flower whose ground goes drops.

The server times every break, so a modified client can't mine faster. The client
says when it starts on a block and with which tool. The server lets the break through
only once that block has been mined for long enough with that same tool, give or take
0.2 s for network jitter. The rules are in `src/world/Mining.h`. `MiningTest` checks:

- the times and tools;
- that breaks which weren't mined, or weren't mined for long enough, are refused;
- that the tool and the block must match what was started;
- that creative is instant.

**Particles.** Breaking a block throws out a burst of little cubes cut from its
texture, which tumble, bounce on the ground and shrink away; mining one chips a few
off as you go. Blocks other players break burst too. They're only for show, on the
client.

## Creatures

The server runs the world's creatures, and the players near them see them, smoothed
like other players. The animals keep to themselves; **every enemy is a dwarf**.

- **Deer** wander grassland in herds of one to three. Hit one and it's knocked back
  and bolts away from you for a few seconds.
- **Wolves** roam grassland in pairs (now and then in place of a herd of deer) and
  leave you alone, unless you hit one: then it goes for you for twenty seconds (1.5
  hearts a bite). Right-click a wild wolf with raw venison in your hand to tame it (it
  eats one). A tame wolf wears a red collar and follows you, keeping close, catching up
  if it falls far behind, and staying as long as you're here. It goes for the hostile
  dwarf nearest you (within 10 blocks) and bites it until it's gone. Taming one is the
  Best Friend achievement. `/summon wolf` brings one.
- **Bats** flutter about in the dark of caves, flitting in a new direction every
  second or so and darting away when hit. They're harmless. `/summon bat` brings one.

### Dwarves

Dwarves are short and stocky, bearded and helmeted, and they live in **camps** on the
surface and **mines** underground (see [Dwarf camps and mines](#dwarf-camps-and-mines)).
Each guards its home: it wanders about it, goes for the nearest survival player in
sight (16 blocks), and gives up on anyone who leads it more than 40 blocks from home,
heading back. Creative players, and players in no-clip, are left alone. Daylight
doesn't trouble them, and they don't turn up just anywhere in the dark: they come from
their camps and mines.

- **Dwarf warriors** (red tunics, an axe) close in and hit for one and a half hearts a
  second. 7 hearts. `/summon dwarf`.
- **Dwarf crossbowmen** (green, a crossbow held level) keep their distance, 7 to 14
  blocks off, backing away if you close in and edging sideways, and shoot a bolt every
  couple of seconds, aimed high enough to allow for its drop. Bolts hurt whatever they
  hit (PvP or not). 5 hearts; one leaves you two arrows. `/summon crossbow_dwarf`.
- **Dwarf chieftains** (bigger, purple, a white beard, a horned helmet and gold) hold
  each camp: two and a half hearts a hit, 16 hearts. `/summon dwarf_chief`.
- **Dwarf miners** (a yellow helmet with a lamp, a pickaxe) don't fight: they work
  for whoever pays them (see [Mining posts](#mining-posts)).

A site fills up the first time a player comes within about 70 blocks of it (a camp
with a chieftain, four warriors and two crossbowmen; a mine with four warriors and
two crossbowmen), then gets one dwarf back every 30 seconds for any killed.

Left click a creature in front of you (within 4.5 blocks) to hit it. A hand does
1 heart, a shovel 1.5, a pickaxe 2, an axe 2.5 and a sword 3.5. Hits land at most
about three times a second, and the server checks the reach. A deer has 4 hearts.
Aiming at a creature doesn't mine the block behind it. Defeating a hostile dwarf is
the Dwarf Slayer achievement.

**Horde nights.** One night in four (decided at dusk, and announced: "The ground
stirs... a horde is coming tonight"), a dwarf **war band** comes for the players:
warriors and crossbowmen turning up on open ground under the sky (never in the caves
below, however high or low the ground is there) 24 to 40 blocks from each player,
up to fifteen at a time, with no home to go back to, hunting from 32 blocks off. It
ends at dawn. Walls, gates, traps and lamps are worth having. `/horde` says whether
tonight is one; operators can start or end one with `/horde on` and `/horde off`.

**Traps work on creatures too** (all but bats, which fly): they press pressure plates
(setting off whatever's on the plate's channel, a dungeon flood chamber included),
spikes hurt them as they hurt players, and crumbling bricks give way under them. A
player sensor can be set to sense hostile dwarves as well as players (see
[Signals](#signals)), so a dwarf at the gate can shut it or light a warning lamp.

In normal worlds creatures spawn by themselves around each player: deer (or wolves) on
open grass 20 to 48 blocks away, up to 6 nearby; bats in dark caves; dwarves at the
camps and mines nearby. They go when no player is within 96 blocks, and they aren't
saved: a world fills with them again as you play. Empty worlds have none of their
own; `/summon` brings any of them. Their models are boxes like the players'; their
legs swing as they move, and they flash red when hit. Hostile dwarves grunt now and
then. The rules are in `src/world/Creatures.h` and the server side in
`src/server/Creatures.cpp`.

`CreatureTest` checks:

- summoning, and that both players see creatures fall onto the ground;
- hits: out of reach, too fast, knockback and fleeing, kills;
- a dwarf warrior hunting a survival player, ignoring a creative one, and not minding
  daylight; a crossbowman shooting from a distance and keeping it;
- a tame wolf seeing off a dwarf warrior, and `/creatures clear`;
- in a normal world, deer turning up by day, and dwarves coming out at the nearest camp
  or mine when a player comes near it.

### Dwarf camps and mines

The world is split into 160 by 160 regions, and about a third hold a dwarf **camp**
and a quarter a **mine**, on dry ground that's flat enough, below the bare peaks.

- A **camp** is a ring of trodden earth inside a palisade of log posts (two or three
  high) with a gap for its gate, three plank huts round a campfire, and a chest of
  camp loot (bread, venison, arrows, iron, hide, sometimes steel).
- A **mine** is a timber headframe over a shaft three blocks across with a ladder down
  20 blocks, and two to four tunnels three wide and three high running out from its
  foot, framed every five blocks by log posts in the walls and a beam overhead, a lamp
  in every other beam, coal and iron ore showing in the walls, and a chest at the end
  of each tunnel (coal, iron ore and ingots, TNT, sometimes steel).

- One camp in four is a **market** instead: the same palisade, huts and fire, with a
  lamp on a post either side of the gate and no loot, where friendly dwarves live (see
  [Markets](#markets)).

- A third of the mines are **deep mines**: their shaft carries on down past the
  tunnels, walled in stone brick with a ladder all the way and a lamp every 12 blocks,
  to a chamber in the Deep (12 blocks into it): the easy way down.
- Half the mines have a **forge** at the end of their first tunnel: a room of fire
  bricks, lit by lamps and two pools of magma, where a **forge golem** stands guard
  over a chest of steel, gold and coke. A golem is a boss of stone and iron with a
  glowing core (45 hearts, hits for 3.5, hardly budges): beat it and it leaves its
  **forge core** (what a steam turbine needs) and 3 steel ingots. Like a king, it
  doesn't come back (`kings.txt`). `/summon forge_golem` brings one.

The **minimap** marks the sites within 480 blocks: a camp as a red triangle, a mine
as a brown square (grey once its golem's fallen), a market as a green dot and a
stronghold as a purple diamond (grey once its king's fallen); those beyond the map's
edge sit on it, smaller, showing the way.

The layouts are in `src/world/Dwarves.cpp`; like dungeons, each chunk stamps just the
part of a site inside it. `BiomeTest` checks camps have their fire, chest and
palisade, and mines their open shaft, ladder, tunnels and chests.

### Raids

Dwarves don't only wait in their camps. On a night that isn't a horde night, now and
then (one night in four, for each survival player within 48 blocks of their bed) a
**raid** comes: "War drums in the dark... a dwarf raid is coming for your home!" Three
warriors and a crossbowman come out of the dark 20 to 30 blocks off, all from one
side, and make for your bed, fighting anyone they find. Walls slow them only so much:
pushed up against planks, a door, glass, logs, leaves or a ladder for a second and a
half, they hack through it (at their feet and head). Stone, bricks and the like hold.
At dawn they go home. `/raid <player>` (operators) sends one now. `CreatureTest` raids
a bed in a plank hut: the band comes, breaks in, reaches the bed, and leaves at dawn.

### Strongholds and the Dwarf King

Rarely (about one region in 25), a dwarf **stronghold** lies 28 blocks under the
ground. A gatehouse of stone bricks with a lamp on each pillar stands over a stairway,
three wide and a step a block, that leads down to a great hall: 21 blocks square and 8
high, mossy brick, four great pillars with lamps, a plank walk to a throne on a dais at
the far end, and walls thick with iron and coal ore. Behind the throne a doorway opens
on the vault: two chests of steel and iron ingots, TNT, arrows and bread. Watch the
foot of the stairway: its floor is crumbling bricks over a pit of spikes.

The **Dwarf King** sits there, with six warriors and three crossbowmen. Half again as
tall as a dwarf, in purple and gold with a crown and a great hammer, he has 60 hearts,
strikes for 3 (4 once he's down to half, and faster), hardly budges when hit, and every
12 seconds (8 when enraged) calls up two more warriors, up to six at once. He won't
follow you more than 18 blocks from his throne. Near him, his health shows as a bar
across the top of the screen.

Defeat him and his hoard is yours (6 steel and 8 iron ingots), everyone's told, and
you earn **Kingslayer**. He doesn't come back: the strongholds whose king has fallen
are kept in the world's `kings.txt`. `/locate stronghold` finds the nearest, and
`/summon dwarf_king` brings one, who keeps to where he was brought (and isn't counted
as a stronghold's). `BiomeTest` checks a stronghold's
hall, throne, vault and stairway; `CreatureTest` fights a king to the end.

### Markets

Not every dwarf is out for blood. A market's dwarves (a **trader** in green with a
broad hat, and three **guards** in blue with shields and spears) are friendly: the
guards keep to the market and cut down hostile dwarves that come within 16 blocks of it.

Right-click the trader to trade (from within 6 blocks). **Gold** (gold ingots, smelted
from the Deep's gold ore) is their money:

| They sell | | They buy | | Barter | |
|---|---|---|---|---|---|
| 1 gold | 2 iron ingots | 8 glass | 1 gold | 4 bread | 1 iron ingot |
| 2 gold | 1 steel ingot | 2 steel ingots | 1 gold | 3 iron ingots | 1 steel ingot |
| 1 gold | 6 bread | 8 crushed iron | 1 gold | | |
| 1 gold | 12 arrows | 4 coke | 1 gold | | |
| 2 gold | 2 TNT | 16 coal | 1 gold | | |
| 3 gold | a guard for ten minutes | 4 hides | 1 gold | | |

**Prices drift.** Each time anyone takes an offer, what it costs goes up by a quarter
of its first price (up to four times it), and comes back down a step every minute: sell
them a lot of glass and they'll want more of it for their gold; buy up their steel and
it costs more for a while. The trade window shows the prices as they are, marking those
that have gone up.

A **hired guard** wears a yellow band. They follow you (catching up if left behind)
and fight the hostile dwarf nearest you within 12 blocks, until their ten minutes are
up and they go home. You can have two at once.

Hit any of a market's dwarves and the whole market turns on you for five minutes: the
guards come after you (within 40 blocks of it) and the trader won't deal with you.
`/locate market` (or `camp`, `mine` or `stronghold`) says where the nearest one is. The offers are
in `src/world/Trades.h`; `/summon dwarf_trader` and `/summon dwarf_guard`
bring them (a market of their own where they're summoned). `CreatureTest` checks the
prices, buying and selling, a price going up, a refused trade, hiring a guard who
follows and fights, and the grudge.

### Mining posts

Dwarves can be hired. A **mining post** (4 planks and an iron ingot) is a chest:
right-click to open it, and put their pay in it: cooked food (bread, cooked fish or
cooked venison). While it holds pay, up to two **dwarf miners** come to work it, one
every few seconds, and wander about it. Every five seconds each digs out the nearest
coal or iron ore within 12 blocks of the post (in any direction, underground too) and
puts it in the post, for a quarter of a block of food. Left unpaid for 30 seconds,
they leave; break the post and they go at once. Look at a post to see how many miners
it has and what they're doing. A post joins item pipes like a chest, so an extractor
can carry the ore away. `FactoryTest` checks an unpaid post gets no miners, a paid one
does, and they dig the ore near it into the post for a quarter of a block of bread
each, and leave when it's broken.

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

## Bows and arrows

A **bow** (1 plank + 1 hide) is a tool; **arrows** come four at a time from half a
plank and a quarter of stone. With the bow in your hand, right-click to shoot an arrow
where you're looking (one arrow each, at most about every 0.6 s; creative players
need none). The server flies it: fast, with gravity pulling it down, until it hits a
block (where it sticks for six seconds), a creature or a player. A hit does 2.5 hearts
and knocks the target back; players only when PvP is on (see [PvP](#pvp)), and armour
helps. Everyone near sees arrows in flight and stuck in the ground. An iron or steel
bow doesn't hit harder (the arrow does the work). `CreatureTest` checks a
hit from ten blocks off, that a bow's shots are spaced out, and arrows sticking in
the ground.

## Ores and iron

The stone underground holds veins of ore: **coal ore** from 8 blocks below the surface
(about 3% of the stone) and **iron ore** from 24 down (about 1%, in smaller veins).
Both want a pickaxe (iron ore is harder). Coal ore breaks into **coal**; iron ore is
smelted into **iron ingots** (1 iron ore + 0.25 coal). Coal also makes a lamp more
cheaply (0.5 glass + 0.25 coal).

An iron ingot makes a stone tool into an **iron** one: hold the tool and make the
**iron upgrade**. An iron tool mines faster (6 times a hand's speed on what it's for,
instead of 4) and hits a heart harder (an iron sword does 4.5 hearts). Each tool is
upgraded on its own. `MiningTest` checks where ore lies, what it breaks into, and what
iron does.

**Steel** comes next, and only from machines: a **smelter** (see
[Air power](#air-power)) turns an iron ingot and half a coal into a **steel ingot**;
there's no recipe for it by hand. A smelter also gets twice the iron out of ore that
crafting does, so the way on is: iron by hand for a compressor, a water wheel and a
smelter, then the smelter's iron and steel. A steel ingot makes an iron tool in hand
into a **steel** one (the **steel upgrade**): it mines 9 times a hand's speed and hits
harder again (a steel sword does 5.5 hearts). A **drill** takes a steel ingot too, so
it comes after the smelter. `CraftTest` checks a tool goes stone to iron to steel, one
step at a time, and stays that way; `FactoryTest` that a smelter makes steel from the
iron it smelts and the coal a drill digs.


**Copper.** Copper ore runs through the rock from 6 blocks down, commoner than iron.
Smelted, an ore gives two copper ingots (or, by hand, an ore and a quarter of a coal
make one). Every signal block is wired with it: a lever pair or a signal lamp takes a
quarter of an ingot, a pressure plate an eighth, each sensor a quarter, a logic gate
half, and a machine sensor half (with half an iron ingot). Miners at a mining post dig
it too, mine chests hold some, and traders sell four ingots for a gold.
`CraftTest` checks every signal block needs copper; `BiomeTest` that it's in the rock.

**Limestone, cement and concrete.** Limestone lies in wide flat beds in the rock,
from 3 to 48 blocks down: pale, banded, and mined with a pickaxe. Burnt, it's
**cement** (an item): a smelter turns a limestone into a cement, or by hand two
limestone and half a coal make one. A cement, two gravel and a sand make four
**concrete**, a smooth grey building block that's slow to break (hardness 6) and can
be chiselled like any building block. `CraftTest` checks the smelting and both recipes;
`BiomeTest` that limestone is in beds in the rock and none far down, and gravel in pockets.

**Gravel** lies in small pockets just under the topsoil (2 to 30 blocks down), dug with a
shovel; it goes into asphalt and concrete.
## Farming

Breaking tall grass now and then gives **seeds** (three times in ten). With seeds in
hand, right-click the top of a dirt or grass block to plant one: a young **crop**
comes up, and ripens into **wheat** on its own, after about a minute and a half on
average (while its chunk is loaded, which it is near a player). Break ripe wheat to
harvest it: you get the wheat and one or two seeds to plant again. Breaking a young
crop just gives its seed back, and water washes crops away. Three wheat bake into
**bread** (6 food), and four bind into a **hay bale**: soft to land on (a fall onto one
doesn't hurt, from any height), though it burns as readily as a bed. Breaking leaves now and then gives a **sapling** (a pine sapling from pine
needles); planted on dirt or grass it grows into a tree after about two minutes: a
trunk four to six logs tall with a round crown of leaves, or a cone of needles for a
pine, if there's room above it. So wood grows back. The server keeps a list of growing crops, saved with the world
(format 9). `FoodTest` checks planting (not in mid air), ripening, harvesting, bread
and eating it.

## Boats

A **boat** (3 planks) takes you across water fast. With it in hand, right-click
water within 4 blocks to sit in it on the surface: steer as you'd walk (6.5 blocks a
second, 9 sprinting), gliding rather than stopping dead; the bow stops at the shore,
since a boat stays on water. Ctrl gets you out (into the water). The boat isn't used
up: you carry it. Others see you sitting in a little wooden boat, and nothing you do
in one counts as a fall. `ShapeTest` checks gliding over water and stopping at the
shore.

## Fishing

A **fishing rod** (a plank and a quarter of a hide) is a tool. With it in hand,
right-click water within 12 blocks to cast: a red and white bobber floats on the
surface. Somewhere between 4 and 14 seconds later a fish bites, with a splash, and
lands in your inventory as **raw fish** (2 food); a little wood cooks it into
**cooked fish** (6 food). Walk more than 3 blocks from where you cast and the line
comes in empty; right-click again to reel in. The server runs it all. `FoodTest`
checks casting (not onto dry ground), the bite, the catch and cooking it.

## Swords and armour

The **sword** is a tool made for fighting: 3.5 hearts a hit, against creatures or
players (an axe does 2.5). It's also quick on leaves. Make it from 0.5 planks and 2
stone.

**Armour** is made from deer **hide** (each deer you kill gives one, along with its
venison). There are three pieces, each worn in an armour slot of its own (see
[Inventory](#inventory)):

| Piece | From | Takes off |
| --- | --- | --- |
| Hide cap | 2 hide | 12% |
| Hide tunic | 4 hide | 24% |
| Hide boots | 2 hide | 12% |

All three take 48% off the damage from hits (creatures and other players) and traps,
rounded, but a hit always does at least half a heart. Armour doesn't help against
drowning, thin or stale air, or starving. The inventory shows the armour slots (an
empty one shows its piece faintly) and the total. A piece crafted goes straight on if
its slot is free, else into your slots; drag it onto its slot (or Crouch-click it) to
wear it, and off again to take it off. Armour is saved with you; operators can hand it
out with `/give cap|tunic|boots [count] [player]`. The rules are in
`src/world/Armour.h`. `CraftTest` checks making the sword and the armour, the damage
sums, and that they're kept after a restart; `CreatureTest` checks that armour takes a
sword hit from 3.5 hearts down to 2.


**Shield, steel plate and crossbow.** A **shield** (2 planks and an iron ingot) and a
**steel plate** (4 steel ingots and 2 hides, worn over the tunic) are armour too: the
shield takes 12% off, the plate 25%, and all the armour together at most 80%. Crouch
with a shield to block: half of each hit's damage (after armour) is stopped. Others see
the shield on your left side and the plate over your chest. A **crossbow** (a steel
ingot, 2 planks and a hide) is held like an item: right-click shoots a bolt (an arrow)
for 4.5 hearts, faster and flatter than a bow's, but it takes 1.2 seconds to wind.
`CreatureTest` checks a bolt's damage, blocking with a shield, and the plate.
## Signs

A **sign** (two from a plank) is a post with a board. Place one and a box opens to
write a line on it (up to 64 characters; Enter finishes, Esc leaves it as it is);
right-click a sign later to rewrite it. Its words float over it like a little plaque
for anyone within 12 blocks who can see it. The server keeps every sign's text, sends
them all to each player who joins, tells everyone when one changes, and saves them
with the world (format 10). `ChestTest` checks that writing reaches the other player
(unprintable characters dropped), that there's no writing without a sign, and that it
survives a restart.

## Ladders

A **ladder** (three from a plank) is a see-through frame of rungs filling its cell.
Walk into one to climb: moving or holding jump takes you up, crouching holds you
where you are, and letting go slides you down slowly. A ladder ends a fall (no fall
damage on one), water doesn't wash it away, and it's fastest to break with an axe.
Stack them up a shaft or against a wall. `ShapeTest` checks climbing, holding on and
sliding.

## Campfires

A **campfire** (a log and a quarter of stone) burns almost as bright as a lamp, with
smoke curling up from it. Right-click it with raw venison or raw fish in your hand to
cook one on the spot, no crafting needed. Standing in it burns (half a heart every
0.75 s), and water puts it out. `FoodTest` checks its light and the cooking.

## Doors

A **door** (2 planks) stands two blocks tall: placing one needs room above for its
upper half and a whole solid block under it (not a slab, a slope or a pipe), and costs
one door. Right-click either half (within 6 blocks) to open
it, which turns both halves into an open frame you can walk through, and again to
shut it. Two doors side by side in a doorway (facing the same way or each other's)
are a double door: opening or shutting either does both. A door is hinged on one side:
put up beside another door it takes the far side from it (so a pair swings apart like
real double doors), and otherwise the side of the doorway you clicked nearer. Crouch
and right-click it (with no block in hand) to swap its hinge to the other side. If the
block under it goes, the door falls and drops as a door. It won't shut on someone
standing in the doorway. Breaking either half takes
the whole door, and gives it back. `BedTest` checks placing, opening, not shutting on
someone, closing from the other half, double doors, refusing a door on a pipe or in
mid air, a door falling when its block goes, mirroring, and breaking.

## Chests

A **chest** (crafted from 2 planks) stores things: thirty slots of stacks, like
yours (see [Inventory](#inventory)). Right-click one within 6 blocks to open it
(Crouch-right-click builds against it instead). The window shows its slots above
yours: click or drag stacks between them, right-click for half or one, and
Crouch-click to move a stack across. I or Esc closes it, and so does walking away.
Anyone can open any chest, and everyone with it open sees changes at once. Breaking a
chest gives what was in it to whoever broke it (if it fits in their inventory).
Chests can't be shaped, and they're fastest to break with an axe. Machines that work
from chests (drills, smelters, crushers, crafters, harvesters, extractors, mining
posts) stop when the chest they'd fill is full, and say so ("its chest is full"). A
chest has offcuts too, from what machines put in it.

The server keeps what's in every chest and saves it with the world (world format 8;
stacks from format 14).
The client is only told what's in a chest while it has it open. `ChestTest` checks:

- crafting and placing a chest;
- opening it within reach and not from afar, and nothing going in unopened;
- moving things in and out, and not more than you have;
- moving stacks between slots, swapping, Crouch-click, and armour slots taking only
  their own piece;
- a second player seeing the same contents and taking from them;
- breaking it (it closes, and the contents go to the breaker);
- the chest and its contents after a restart.

## Air power

With a block in hand, right-clicking machinery builds against it (a wheel onto a
shaft, a pipe onto a compressor, a valve or tank onto a pipe); with an empty hand or
a tool, it's used or described instead.

Machines run on **compressed air**. A **compressor** makes it, turned by **water
wheels** through **shafts**:

- **Water wheels** are 3 x 3 blocks standing on edge around their hub (the block you
  place), drawn as a big wooden wheel with spokes and paddles. The axle is level,
  pointing the way you face when you place it. The eight cells around the hub in the
  wheel's plane are its paddles: they must be clear (air, water or plants), or the
  wheel is jammed. A wheel turns when water flows down **one side** of it: the server
  measures the water the simulation moves through the three paddle cells on each
  side, and the wheel turns with the difference, faster the more water goes by (full
  speed at a block of water a second). Water pouring past one side drives it; water
  through the middle, underneath, or down both sides alike doesn't, and still water
  never does.
- **Shafts** lie end to end along the same axis as the wheels' axles, through any
  number of wheels' hubs; shafts and wheels on a line turn together, drawn as a
  turning wooden rod. A **compressor** at either end of the line takes the power of
  every wheel on it: 16 units of air a second from each wheel at full speed.
- **Air pipes** join face to face to each other, to compressors, machines, air tanks
  and valves; everything joined is one network sharing one store of air. Every pipe,
  compressor and machine holds a little (16 units), and the network's **pressure** is
  how full it is, so long pipe runs store more but take longer to fill.
- An **air tank** holds 256 units: a store of pressure, filling while the wheels make
  more air than the machines use and keeping them going when the water stops. (It's
  more to fill, too: a network with a tank takes longer to reach running pressure.)
- A **creative air tank** (magenta; creative only) keeps its whole network full,
  however much air the machines use, with no wheels or compressor needed. It has no
  recipe, survival players can't place one, and breaking one in survival gives nothing.
- A **valve** joins the pipes either side of it only while it's open; shut, the two
  sides are separate networks, each keeping its own air. Right-click (with an empty
  hand or a tool) opens and shuts
  it, or give it a channel (see [Signals](#signals)) and it opens while that's on. It's
  drawn as a length of air pipe with a red handle on top, turned along the pipe when
  open and across it when shut. Breaking a pipe lets
  out what it held, and the two halves of a network keep their pressure. Pipes are
  drawn as thin tubes, joined to what they connect to (they still stop you like a
  whole block). Machines run above 25% pressure.

The machines:

- The **drill** digs straight down from under itself, one block every 1.5 seconds for
  12 units of air, up to 32 blocks deep, and puts what it digs into a **chest
  touching it**, or one at the end of **item pipes** from it. It stops at water (the shaft would flood), at chests, beds, doors,
  signs, TNT, pumps and other machinery, and waits without a chest or without
  pressure. A working drill throws up dust where it digs.
- The **pump** (see [Water](#water)) lifts water on air, cheaply: the air a wheel
  makes from water pouring past it lifts three times that water as high as the wheel
  (3 blocks); anything on the level or downhill counts as a block. One wheel at full
  speed keeps one pump going flat out (2.5 blocks of water a second) lifting 3 blocks,
  so a pump can keep its own wheels turning and lift plenty more besides. Pumps take
  only the air above the running pressure that the drills on their network don't need
  for their next block.
- The **smelter** smelts a block every 2 seconds for 6 units of air, in the chests
  joined to it (touching it, or at the end of item pipes from it), putting what it
  makes back in the same chest: iron ore into **twice** the iron ingots crafting gives
  (and no coal), raw venison and fish into cooked, sand into glass, and logs into
  half a block of coal (charcoal), and last of all an iron ingot and half a coal into
  a **steel ingot** (the only way to make steel; keep iron you want as iron in a chest
  it can't reach). It takes whatever's first in that list that a chest has. It glows while it works. So a drill and a smelter on the same chest, or
  joined to it by item pipes, dig and smelt ore on their own.
- **Item pipes** (thin wooden tubes) join face to face and to chests, drills and
  smelters: a drill or smelter reaches every chest along them (up to 512 pipes).
- An **extractor** faces the way you look when you place it: put it against a chest.
  Twice a second, for 1 unit of air, it takes one block or item (whole: offcuts stay)
  out of that chest and sends it out of its back along item pipes to another chest: one
  behind a filter pipe set to it if there is one, else one that already holds some of
  it (so a row of chests sorts itself by example), else the nearest, with room for it.
  It works through what the chest holds in turn, and leaves what nowhere will take.
- A **filter pipe** (a blue item pipe) lets only one thing through. Right-click it
  with something in hand to set it to that, or empty-handed to let anything through
  (Crouch-right-click builds against it). Drills and extractors send things through a
  filter only if it passes them; smelters and crafters reach only through unset ones.
  Look at one to see what it passes.
- A **crafter** makes one recipe over and over: right-click it with what it should
  make in hand to choose (empty-handed, it says what it makes). Every 2 seconds, for 4
  units of air, it takes what the recipe needs from a chest joined to it (touching
  it, or along item pipes) and puts what it makes back in the same chest. It won't
  make tools, armour or upgrades.
- A **harvester** reaps ripe wheat within 4 blocks of it (and a block up or down),
  one crop every 1.5 seconds for 3 units of air, replanting it from its own seed, and
  puts the wheat in a chest joined to it.
- The **piston** pushes the way you were looking when you placed it (level, up or
  down). Give it a channel (see [Signals](#signals)) and join it to air pipes: while
  the channel is on it pushes the row of blocks in front of it (up to 12) a block on,
  for 4 units of air, and holds them out with its head, a plate on a rod. A row ending
  in water pushes the water up or aside (it won't push if there's nowhere for the
  water to go), and plants at the end are crushed. It won't move chests, beds, doors,
  signs, crops, ladders, signal blocks or machinery. When the channel goes off it
  pulls its head back, bringing the block in front of the head with it, so a piston
  makes a door out of any block. Breaking the piston takes its head too; the head
  itself gives nothing back. A **pressure sensor** (see [Signals](#signals)) joins
  the pipes too, and sends while the pressure is high enough.
- The **air launcher** faces the way you were looking when you placed it, like a
  piston. Give it a channel and join it to air pipes: each time the channel comes on
  it fires once (it needs the running pressure), throwing whatever is in the cell in
  front of it (players, creatures and items lying there) that way, with a little lift
  if it's fired sideways so it carries. Its power, set in its signal window, is how
  hard it throws: 4 to 32 blocks a second (0, the default, is 18). A throw at 18 takes
  12 units of air, and harder throws take more in proportion. It
  fires with a pneumatic thunk and a puff of air. To fire again, switch the channel
  off and on. The server gives a thrown player a few seconds' leeway in its movement
  checks, but they land as hard as they land.

A **pressure gauge** is a block you fix to an air pipe, tank, compressor or anything
else on a network: place it against one and it faces out from it, a dial on a short
stem, its needle swinging from empty to full with the network's pressure (the
running pressure, a quarter, is marked in green). It reads the network but isn't part
of it, so it doesn't join what's either side of it; one fixed to nothing reads
nothing. Right-click any part (wheel, compressor, pipe, pump, drill, tank...) with an
empty hand or a tool for its menu: its name and one short line of how it's doing (a
word or two for its state, its numbers, and its network's air pressure, such as
"working, 12 deep, air 80%").

Messages are kept short: what you get for a kill shows in your inventory rather than
the chat, and only big moments (a boss falling, a raid coming) are announced.

Recipes: a water wheel from 6 planks and a log; 3 shafts from a log; a compressor
from an iron ingot and 2 stone; 4 air pipes from a stone and a quarter of an iron
ingot; an air tank from 2 iron ingots and half a glass; a valve from half an iron
ingot and a stone; a drill from a steel ingot, an iron ingot and 2 stone; a piston from a planks, a
stone and half an iron ingot; an air launcher from a piston and an iron ingot; a pressure gauge from a quarter of a glass and a quarter of an iron ingot; a smelter
from 4 stone bricks and an iron ingot; 4 item
pipes from a planks and a quarter of an iron ingot; an extractor from an iron ingot and
a planks; a filter pipe from an item pipe and a quarter of an iron ingot; a crafter
from a steel ingot, an iron ingot and 2 planks; a harvester from half a steel ingot
and 2 planks.

The server runs it all four times a second (`Server::UpdatePower`,
`src/server/Power.cpp`; the rules and numbers are in `src/world/Power.h`): the water
simulation's changes are counted in each wheel's paddle cells as they happen
(`ServerWorld`), and pumps get an allowance of lift for the next tick
(`WaterSimulation::SetPumpBudget`) and report what they used. Players within 96
blocks are told how each wheel and machine is doing (`Machines`, protocol 34).
`PowerTest` builds a mill in an empty world (a raised reservoir pouring through two
holes down one side of two wheels on one shaft, into a pit; a compressor; air pipes
to a drill and a pump) and checks:

- wheels with no water flowing stand still, with no air, and the drill and the pump
  wait for pressure;
- a block in a wheel's paddles jams it, and clearing it frees it;
- water pouring down one side turns the wheels, both wheels on the shaft turn the
  compressor, and the network fills with air;
- the pump runs on the air, and the drill digs into the ground with what it digs in
  the chest;
- right-clicking the compressor and a wheel describes them;
- an air tank behind a shut valve gets no air, and opening the valve lets the
  network's air into it;
- cutting the air pipe starves the drill;
- stopping the water brings the wheels to a stop.

`FactoryTest` builds a drill over a column of sand and iron ore, item pipes from it to
a chest, a smelter on the chest and air pipes to both, and checks that the drill's
sand comes down the item pipes into the chest, the smelter turns both blocks of sand
into glass and both of iron ore into four iron ingots, glowing while it works and
going dark when there's nothing left, and that right-clicking it describes it. Then
an extractor on that chest sorts it along item pipes: a filter pipe, set by
right-clicking it with glass in hand, takes the glass to one chest and keeps the iron
and steel out, which go to another. A crafter on that chest, set to stone bricks,
turns the stone put in it into bricks block for block, and a harvester beside it
reaps two ripe crops of wheat into it and replants them. Last, a pressure gauge fixed
on top of an air pipe reads the network's pressure, one on bare ground reads nothing,
and right-clicking the gauge describes the network.

## Oil and fluids

**Oil** lies deep underground, in pockets of **oil shale** (dark rock streaked with
crude) between 30 and 80 blocks below sea level, in broad oil fields that are rare: about
3% of the world has oil under it, so it takes some looking for. Over a field, here and there a block of the ground is a black **tar seep**:
the sign to drill. `/locate oil` (operators) says where the nearest is.

**Fluid pipes** (thin grey tubes) join face to face and to **fluid tanks**; everything
joined is one network holding one fluid at a time: water, crude oil or fuel (it takes
the kind of the first fluid put in, and nothing else until it's empty). A pipe holds 64
units and a tank 2,048 (a block of water is 128). Machines join a network from beside
it; they aren't part of it, so they don't join what's on either side of them:

- An **oil derrick** structure (see [Multiblock structures](#multiblock-structures))
  pumps crude oil up out of the shale under it into the fluid pipe at its crude port.
  The well is its own: there's no need to dig down. Each shale block gives about 4,000
  units, then it's spent (stone).
- A **refinery** structure takes crude oil from the pipe at its crude port and puts the
  same amount of **fuel** into the pipe at its fuel port.
  What's left over is **bitumen**: one for every 256 units of crude it refines, into the
  refinery's own inventory (right-click it to take it out). One bitumen, two gravel and a
  sand make four **asphalt**, a dark road block that you walk (or sprint) 30% faster
  on. It can be chiselled like any building block.
- An **engine** is part of an air network, like a compressor: it burns 16 units of
  fuel a second from the pipes beside it and adds 24 units of air a second to its
  network (more than a water wheel), so oil powers machines anywhere, no river needed.
  One derrick keeps two engines going.
- A **drain** (the iron grate you craft) takes water from the cells above and beside it
  into the fluid pipes beside it, only as much as they have room for, so none is lost
  (a natural sinkhole, where a river ends, still takes it out of the world).
- A **fluid outlet** faces out from what you place it on, and lets water from the pipes
  beside it out into the cell in front of it, as much as there's room for. So a drain,
  pipes and an outlet move water anywhere, uphill or down. To turn one on and off, put
  a fluid valve in the pipe before it.
- A **fluid valve** (half an iron ingot and a fluid pipe) joins the fluid pipes either
  side of it only while it's open. Shut, it keeps the two sides apart and closes the
  pipes' ends there, so nothing spills or vents. It opens and shuts by hand or by a
  channel, like the air valve, and looks like a length of fluid pipe with a red handle.

A pressure gauge fixed to a fluid pipe or tank shows how full its network is, and
looking at a tank or a gauge says of what. Right-clicking any of them (empty-handed)
describes the network. What pipes and tanks hold is saved with the world (format 12).

Recipes: 4 fluid pipes from a quarter of an iron ingot and a quarter of a glass; a
fluid tank from 2 iron ingots and a glass; a fluid outlet (or a drain) from half an
iron ingot and a stone; an oil derrick from 2 steel ingots, 2 iron ingots and 2 planks;
a refinery from 2 steel ingots, 4 stone bricks and an iron ingot; an engine from a
steel ingot and 2 iron ingots.

The rules are in `src/world/Fluids.h`; the server runs it with the air
(`Server::UpdatePower`, `src/server/Power.cpp`), and drains and outlets trade with the
water simulation through allowances, like pumps (`WaterSimulation::SetDrainBudget`).
`FactoryTest` builds a derrick over oil shale, pipes to a tank, a refinery into another
tank and an engine on that with an air network of its own, and checks crude oil
reaches the first tank, fuel the second (with bitumen left in the refinery), the engine
runs and fills its network, and the
shale records what's been drawn; then a drain under a cup of water, piped through a
tank to an outlet, and checks the cup empties and the water comes out of the outlet.
`SaveTest` checks what pipes and tanks hold is saved. `PumpTest` checks a drain with no
room in its pipes takes nothing, and with room for 40 units takes just those.


**Machines have their own inventory.** A smelter, drill, crafter, harvester, crusher,
boiler, and any formed structure (click any of its blocks) opens like a chest:
right-click it with an empty hand or a tool (it also says how it's doing). Put things
in and it works from them, putting what it makes back in; no chest needed. Chests
touching it or joined by item pipes still work as before: a machine uses its own
inventory first, except a drill or harvester, which fill a joined chest if there is
one and their own if not. Extractors take from machines' inventories too, and send
into them along item pipes. With a block in hand, right-clicking a machine still builds
against it (and a crafter still takes what's in hand as its recipe). Breaking a machine
gives you what's in it. `FactoryTest` checks a smelter with no chest smelting sand in
its own inventory.
### Steam power

Between water wheels and diesel there's **steam**. A **boiler** (4 iron ingots and 2
fire bricks) takes water from the fluid pipes or tank **under** it and puts steam into
the pipes or tank **over** it, 32 units a second, while it has fire: it burns a coal
from the chests joined to it every 8 seconds (a coke lasts 16). A **steam turbine** (3
steel ingots and a **forge core**, from a mine's forge golem) is part of an air
network, like an engine: it uses 32 steam a second from fluid pipes beside it and
puts 40 air a second into its network. A drain or a pump feeds the boiler its water.
`FactoryTest` checks a boiler with no coal says so, boils with it, and drives a
turbine that fills its air pipes.

Steam escapes into the air (see [Lava and steam](#lava-and-steam)), the way a pump's
water spills. It comes out of every open end of its pipes (a pipe joined to just one
thing), from the end face only, and out of the front of every fluid outlet on its
network. An open end vents up to 32 units a second and an outlet twice that, 64 (the
way to let steam out on purpose), and each unit becomes 10 units of steam in the air. An end with a block in front of it is closed, so close every end of the
pipes that carry steam to a turbine, or it goes up in a cloud. `/fluid x y z steam`
(operators) fills a pipe with steam. `SteamTest` checks steam venting from both ends
of an open pipe, and from an outlet once the ends are closed.

## Multiblock structures

The bigger machines are **structures**: built from a pattern of blocks round a machine
block, then formed by striking any block of it with an **engineer's hammer** (an iron
ingot and half a planks: hold it and right-click). If the pattern isn't finished, the
hammer says how many blocks are missing or wrong and where the first one is, and what
it needs. Formed, the machine block becomes the structure's core and the rest its
parts, drawn as one big model; its **ports** stay as the pipes they are, so you join
air, fluids and items to it the usual way. Break any part of it and it comes apart,
every block back as it was (the one you broke given up as usual). Patterns can face
any of the four ways. `/form x y z` (operators) does what the hammer does.

Each is 3 by 3 across. Layers from the ground up, with each layer's rows from back to
front (`.` is anything):

| Structure | Pattern | What it does |
|---|---|---|
| **Oil derrick** | `FFF / ADL / FFF`, then corner frames two high, then `.F. / FFF / .F.`, then one frame on top. F steel frame, D oil derrick, A its air pipe, L its crude pipe | pumps crude oil from the shale under its whole footprint, down to 160 blocks, into the fluid pipe at L: three times what a derrick block alone used to (48 units every half second, for 3 air) |
| **Refinery** | `CAC / IRO / CCC`, then `TCT / CCC / TCT`, then `.C. / CCC / .C.`. C machine casing, T fluid tank, R refinery, A its air pipe, I crude in, O fuel out | crude oil in at I, fuel out at O, up to 96 units each update (three times as much), for 4 air |
| **Blast furnace** | `BAB / BBB / BBB`, then `BHB / BSB / BBB`, then `BBB / B.B / BBB`. B fire bricks, S smelter (inside), A its air pipe, H its item pipe | smelts from the chests along its item pipe, a block every half second (four times a smelter), for 3 air |
| **Coke oven** | `SSS / HXA / SSS`, then `SSS / SSS / SSS`, then `... / .S. / ...` (its chimney). S stone bricks, X smelter (inside), H its item pipe, A its air pipe | bakes 2 coal into a coke every 3 seconds, from the chests along its item pipe, for 3 air |
| **Diesel compressor** | `MAM / FEM / MMM`, then `TMT / MMM / TMT`. M machine casing, T fluid tank, E engine, A its air pipe, F its fuel pipe | burns 32 fuel a second from the fluid pipes at F and puts 72 air a second into the air network at A: three times an engine's air for twice its fuel |

An oil derrick or refinery block on its own does nothing now: it's the heart of its
structure. (A smelter still works alone; the blast furnace is its upgrade.) Their
models are in `Resources/Data/structures.json`, in the same format as creatures (see
[Editing blocks and creatures](#editing-blocks-and-creatures)): parts can be drawn
only while it works (the refinery's flare, the furnace's glowing mouth), and its
motions (the derrick's rocking beam) only move then.

**Coke** makes steel for a quarter of a block's worth instead of half a coal's, so a
smelter or blast furnace with coke in its chests uses it first.

Recipes: 4 steel frames from a steel ingot and an iron ingot; 2 machine casings from
half a steel ingot and 2 stone; 2 fire bricks from 2 stone bricks and a quarter of a
coal. The patterns are in `src/world/Multiblocks.cpp`, and the server's side in
`src/server/Multiblocks.cpp`. `FactoryTest` checks the hammer names the missing block
of an unfinished blast furnace, forms the finished one (its pipes kept as ports), that
it smelts four sand in well under the time a smelter takes, that breaking a part takes
it apart, that derrick and refinery structures pump crude and refine it into fuel
for an engine, that a coke oven bakes coal into coke, and that a diesel compressor
fills its air network once its tank has fuel (`/fluid x y z fuel`, operators, fills
a fluid pipe or tank with water, crude or fuel).

### The crusher

A **crusher** (a steel ingot and 2 stone bricks) is an air machine like the smelter:
from the chests joined to it, every 2 seconds for 4 air, it grinds an **iron ore into
two crushed iron**, or a stone into sand. Crushed iron smelts to two iron ingots like
ore, so ore through a crusher makes four ingots instead of two. `FactoryTest` checks
both.

## Signals

Signals are wireless, on numbered **channels** (1 to 255). Anything sending on a
channel is heard by everything listening to it within **64 blocks**.

| Block | What it does |
|---|---|
| **Lever** | sends on its channel while it's on; right-click flips it. It goes only on the face of a solid block (floor, wall or ceiling), pointing out of it: a stone base with a wooden handle that tips over when flipped. If that block goes, it falls off and drops as an item |
| **Pressure plate** | sends while it's stood on, and for a second after |
| **Logic gate** | listens to one or two channels (A, B) and sends its answer on another: AND, OR, XOR, NOT (of A), RELAY (A again: to carry a signal further than 64 blocks), CLOCK (on and off each second while A is on, or always with no A) or LATCH (A turns it on and it stays on until B turns it off; while B is on, A can't turn it on) |
| **Signal lamp** | lights while its channel is on |
| **Valve** | opens while its channel is on (with no channel, right-click opens and shuts it); see [Air power](#air-power) |
| **Sluice gate** | with a channel, opens out of the way while it's on, letting water through, and closes when it's off, pushing any water in its cell up or aside (it waits if there's nowhere for it to go); with none, it stays shut |
| **Piston** | pushes out while its channel is on and pulls back when it's off; runs on air (see [Air power](#air-power)) |
| **Air launcher** | throws what's in front of it once each time its channel comes on, as hard as its power setting (4 to 32 blocks a second; 0 is 18); runs on air (see [Air power](#air-power)) |
| **Water sensor** | sends while water touches it (an eighth of a block or more, on any side) |
| **Pressure sensor** | joined to air pipes, sends while their pressure is at least its setting (a percentage; 0 for any air at all) |
| **Daylight sensor** | sends by day, while the sky is open above it |
| **Player sensor** | sends while a player is within its range (4 blocks unless set, up to 16); tick "Hostile creatures too" and hostile dwarves count as well |
| **Heat sensor** | sends while fire or lava is within its range (4 blocks unless set, up to 8) |
| **Sprinkler** | sprays the fires round and below it out while its channel is on (with no channel, whenever there's fire in its reach); see [Fire](#fire) |

Placing a lever, a logic gate, a signal lamp, a piston, an air launcher, a sprinkler or a sensor opens a small
window to set its channels (and a sensor's or launcher's setting); right-click a gate or a lamp (with an empty hand or a tool), or crouch and
right-click any signal block
(lever, plate, gate, lamp, valve, sluice gate, piston, launcher, sprinkler or sensor), to change them. Looking at one shows
its channels (and a lever's, gate's or valve's state) under the crosshair. Gates work
out their answer from what they heard and send it from the next update, a tenth of
a second later, so a loop of gates steps along instead of locking up.

Recipes: 2 levers from a quarter of a plank and a quarter of a stone; a logic gate
from a stone, a quarter of an iron ingot and a quarter of a glass; a signal lamp from
a glass and a quarter of a coal. Sensors each take a stone (a planks for the daylight
sensor) and a quarter of an iron ingot, with a quarter of sand (water), glass
(pressure), coal (player) or half a glass (daylight).

The server runs it ten times a second (`Server::UpdateSignals`,
`src/server/Signals.cpp`; the rules are in `src/world/Signals.h`), keeps every
block's channels and saves them with the world (world format 11), and tells everyone
about each block's channels and state as they change (`SignalInfo`, protocol 46).
`SignalTest` checks:

- a lamp lights and goes out with a lever on its channel, and one on another channel
  doesn't;
- signals don't reach 80 blocks, and a relay gate carries them 82;
- AND and NOT gates follow two levers, and a clock gate blinks its lamp;
- a latch gate comes on with A and stays on after it, goes off with B, and won't come
  on while B is on;
- a valve without a channel opens by hand; one on a channel opens and shuts with it
  and won't move by hand;
- a sluice gate on a channel opens, and closes again pushing the water in its cell out
  beside it;
- a pressure plate lights its lamp while it's stood on, and it goes out after;
- a piston with air pushes two stones out and pulls the first back, one without air
  stays put, a chest in the row stops it, and breaking a piston takes its head;
- an air launcher without air doesn't fire; with air it throws the player standing in
  front of it along its facing, only once while its channel stays on, and again after
  the channel goes off and on, and set to 30 it throws at 30 blocks a second;
- player, water, pressure and daylight sensors each come on and go off when they
  should;
- a dwarf penned on a pressure plate presses it, a player sensor set to sense
  creatures sees it (and a players-only one doesn't), spikes kill another, and
  `/horde on`, `/horde` and `/horde off` do what they say;
- (and `DungeonTest`, that every flood chamber's plate sets a latch gate in its
  ceiling that its nine sluice gates listen to, and a lever resets it, each on a
  channel of its own: the server wires them up the first time the plate is stepped
  on);
- a broken block's channels go, and channels are still there after a restart.


**Machine sensor** (an iron ingot and half a glass): place it touching a machine, a
chest, a fluid tank or pipe, or air pipes, and set what it watches for: a machine
**working**, **stopped**, or **waiting for air**; a chest with **anything in it**, or
**empty**; or a tank or air network **at least this full** (a percentage). It sends on
its channel while that's so. With it, factories can run themselves: stop the
extractor when the chest is full, light a lamp when the furnace runs dry, open a valve
when the tank's low. `SignalTest` checks a sensor on a chest lighting a lamp while
there's something in it.
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
| Bread | 6 | crafting: 3 wheat (see [Farming](#farming)) |
| Raw fish | 2 | fishing (see [Fishing](#fishing)) |
| Cooked fish | 6 | crafting: 1 raw fish + 0.25 log |

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

In survival, the inventory (I) has a crafting list below your slots. Each recipe
shows what it takes, and is greyed out until you have it. Make makes one, and x4
makes four. What it makes goes into your slots (if there's no room, nothing is made).
Tools are made of stone; crafted armour goes straight on if its slot is free.

| Makes | From |
| --- | --- |
| 4 planks | 1 log |
| Stone bricks | 1 stone |
| Glass | 1 sand |
| Mossy bricks | 1 stone brick + 1 leaves |
| Lamp | 1 glass + 0.5 planks |
| Pump | 2 stone + 1 glass |
| Drain | 0.5 iron ingot + 1 stone |
| Sluice gate | 1 stone brick + 1 planks |
| Pressure plate | 0.25 stone (pebbles will do) |
| Spikes | 1 stone |
| Pickaxe | 1 planks + 1.5 stone |
| Axe | 1 planks + 1 stone |
| Shovel | 1 planks + 0.5 stone |
| Cooked venison | 1 raw venison + 0.25 log |
| Sword | 0.5 planks + 2 stone |
| Bow | 1 planks + 1 hide |
| Fishing rod | 1 planks + 0.25 hide |
| Cooked fish | 1 raw fish + 0.25 log |
| 4 arrows | 0.5 planks + 0.25 stone |
| Chest | 2 planks |
| 3 ladders | 1 planks |
| Door | 2 planks |
| Campfire | 1 log + 0.25 stone |
| 2 signs | 1 planks |
| TNT | 1 sand + 0.5 coal |
| Boat | 3 planks |
| Bed | 3 planks + 1 hide |
| Iron ingot | 1 iron ore + 0.25 coal |
| Lamp (with coal) | 0.5 glass + 0.25 coal |
| Iron upgrade (a stone tool in hand becomes iron) | 1 iron ingot |
| Steel upgrade (an iron tool in hand becomes steel) | 1 steel ingot |
| Bread | 3 wheat |
| Hide cap / tunic / boots | 2 / 4 / 2 hide |

Amounts are in blocks, using the same units as building (a block is 64): part of a
block comes out of its offcut. Tools are items in your slots, saved with the player,
and you can make as many as you like. In survival you can only use the tools you
carry; without one you work with a bare hand, for mining and for fighting. The server
checks that too. Creative players have every tool and don't need to craft. Operators
can hand out tools with `/give pickaxe [player]` (or `iron_pickaxe`, `steel_pickaxe`).

Two blocks are new with crafting:

- **Planks** are a building block (stepped when carved), fastest with an axe.
- **Glass** is solid, but you can see through it and light passes. Faces behind it
  aren't hidden, and touching panes join without a face between them.

The recipes are in `src/world/Crafting.h` and the server's checks in
`Server::HandleCraft`. `CraftTest` checks:

- the recipes;
- crafting several at once;
- that crafts you can't afford change nothing;
- inventories: stacks, slots filling up, offcuts, a full inventory;
- that a tool is an item (and a second can be made);
- that upgrades work on the tool in hand, stone to iron to steel;
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


**Breathing gear.** An **air diffuser** (2 iron ingots, a glass and a glow fungus) is a
machine on an air network: while the network has the pressure, it uses 1.5 air a
second to breathe out fresh air, and within 8 blocks of it thin and stale air don't
count. Bring a compressor or turbine (or an engine and fuel) down with you and the
Deep is somewhere you can stay. An **oxygen tank** (3 iron ingots and a glass) is worn
on the back: it holds 90 seconds of air, and while anything would take your breath
(stale air, thin air, or water) you breathe from it instead, keeping your breath full;
in good air (the surface, or a diffuser's reach) it fills back up, six seconds' worth a
second. A gauge over the food shows it while it's in use or filling. `BreathTest`
checks a diffuser with and without air, and its reach, and the tank emptying under
water and filling again on land.
## Achievements

Survival players earn achievements for milestones, once each: breaking a first block,
making a tool, bringing down a deer, defeating a hostile dwarf, cooking venison, eating,
harvesting wheat, baking bread, digging out iron ore, the iron upgrade, a full set of
armour, opening a dungeon chest, sleeping through a night, and taking something down
with an arrow, catching a fish, taming a wolf, defeating a Dwarf King, going down into the Deep, trading with a dwarf trader, making a steam turbine, defeating a forge golem, and defeating a raider (twenty-two in all). Earning one shows a banner under the compass for a
few seconds (with a chime) and tells everyone else. They're saved with you.
`/achievements` lists what you've earned and what's still to do. `FoodTest` checks
the ones it passes on the way, that `/achievements` answers, and that each is earned
only once.

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
| `/weather <clear\|rain\|storm\|auto>` | operators | set the weather |
| `/tp <x> <y> <z>`, `/tp <player>`, `/tp <player> <x> <y> <z>` | operators | teleport; `~` means relative (`/tp ~ ~10 ~`) |
| `/save` | operators | save the world now |
| `/kick <player> [reason]` | operators | remove a player |
| `/op <player>`, `/deop <player>` | operators | make or unmake an operator |
| `/noclip [player]` | operators | fly through blocks, on or off (Space / E up, Q down) |
| `/gamemode` | everyone | show your game mode |
| `/gamemode <creative\|survival> [player]` | operators | change a game mode (also `/gm`, `c`/`s`) |
| `/give <block> [count] [player]`, `/give <tool> [player]` | operators | give survival blocks (`count` in blocks, default 64) or a tool |
| `/fill <x1 y1 z1> <x2 y2 z2> <block>` | operators | fill a box with a block (`air` clears it; up to 32768 cells) |
| `/pressurise <x y z>` | operators | fill the air network with a pipe or machine there with air |
| `/form <x y z>` | operators | form the structure a block is part of, as the engineer's hammer does |
| `/locate <spring\|sinkhole\|oil>` | operators | where the nearest spring (a lake's overflow), sinkhole (where its river ends) or oil is |
| `/horde [on\|off]` | anyone to ask; operators to set | whether tonight is a horde night, or start or end one |
| `/summon <deer\|wolf\|bat\|dwarf\|crossbow_dwarf\|dwarf_chief\|dwarf_miner> [x y z]` | operators | bring a creature (default: a few blocks ahead; `~` relative) |
| `/creatures [clear]` | operators | count the creatures, or remove them all |
| `/pvp`, `/food` | everyone | whether players can hurt each other; how fed you are |
| `/achievements` | everyone | the milestones you've reached, and those still to come |
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

The sun is a bright disc with a glow in its direction, and the moon a pale disc
opposite it. As night falls the stars come out and twinkle. Clouds (rain) hide all
three. On clear nights fireflies drift and blink over grassy ground near
you (up to 40 at a time), and go at dawn.

Now and then it rains: clear spells last 10 to 25 minutes and showers 2 to 5, fading
in and out over ten seconds. Rain greys and dims the sky, falls as streaks around
the player, and tops up lakes and pools (see [Water](#water)). It doesn't fall under
cover: the game keeps, for each column round the camera, the top of the highest solid
block over it, refreshed a few rows a frame, and the rain shader leaves out drops
below it. So under a roof or a tree you see it falling just beyond the edge.
Underwater there's none.

While a daytime shower is light (starting or clearing), a **rainbow** stands in the
sky opposite the sun, highest when the sun is low.

Where it's cold (snow, snowy grass, ice or pines underfoot), it falls as **snow**:
white flakes drifting slowly down and swaying, instead of rain.

Three showers in ten are **thunderstorms**: the sky goes darker still, and every 3
to 12 seconds lightning strikes the ground somewhere within 40 blocks of a player. A
strike is a jagged glowing bolt from high in the sky, with a flash that lights up the
whole world for a moment, then thunder that rolls in later the further off it was.
Anyone within 3 blocks of a strike takes three hearts (armour helps). Operators can
call one with `/weather storm`.

`--day-length SECONDS` and `--weather clear|rain|storm|auto` change these, on `VoxelEngine`
(singleplayer or `--server`) and `VoxelServer`. `WeatherTest` checks thin water, that
rain fills a drained pool to the brim and no further while leaving full, covered and
dry-ground water alone, that the clock runs, saves, resumes and can be skipped, and
that a storm reaches the client and strikes near the player.


**Weather and water.** Springs follow the weather: they pour 1.6 times as hard in the
rain and 2.5 times in a thunderstorm, so rivers swell and pools fill; and after two
whole days without rain it's a **drought** (everyone's told), and they slow to a third
until it rains again. `/weather` says how hard they're flowing; `/weather drought`
(operators) starts one. `WeatherTest` checks the storm's flow, a drought and its end.
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


Carrying light: with a lamp or a campfire selected in the hotbar (and some to hold),
the world around you lights up as you walk, falling off a level a block like placed
light. Other players carrying one light their surroundings too (up to eight lights at
once), and figures and creatures near a carried light are lit by it.
## Editing blocks and creatures

Blocks' details and creatures' models are data, in `Resources/Data`, so you can change
them without touching the code. Edit the files there (the build copies them beside the
game), or the copies beside the game for a quick try. The game notices when one of
them changes (it looks once a second) and loads it again, saying so in chat; **R** also
loads them again, with the textures and shaders. A file that can't be read is
reported, with the line, and the game carries on with its own built-in details (blocks)
or plain boxes (creatures).

**One file per entry.** Blocks, creatures and structures are each a folder in
`Resources/Data` (`blocks`, `creatures`, `structures`), with a file for each block or
model named by its id (`creatures/deer.json`, `blocks/stone.json`) holding just that
entry, and a `README.md` there saying what goes in them. Add a model by adding a file;
a model may still inherit from one in another file. (A single `blocks.json`, or
`creatures.json`, of them all is still read if there's no folder.)

**Recipes are data too**: `recipes.json` (crafting) and `smelting.json`, each a list in
order. A crafting recipe is `{ "id": "planks", "name": "4 planks", "takes": { "log": 1 },
"makes": { "planks": 4 } }`, amounts in blocks (or items: 0.25 is a quarter); instead of
`makes` it can make a `tool` (pickaxe, axe, shovel, sword, bow, rod, chisel: of
stone), a piece of `armour` (cap, tunic, boots, shield, plate, tank) or an `upgrade`
(iron, steel: the tool in your hand goes up a tier). Keep
the order and add new ones at the end: a crafter remembers its recipe by its place in
the list. A smelting recipe takes one or two things and makes one, the first a chest
can afford being smelted. A file with a mistake (a block that doesn't exist, nothing
made) is refused, saying where, and the recipes stay as they were.
`VoxelEngine --dump-recipes FOLDER` writes the game's own out afresh. The server reads
them too, so it and its players should share them.

**Blocks** (`blocks/<id>.json`): one per block. Any of these can be changed, and anything
left out (a field, or a whole block) keeps the game's own value:

| Field | What it is |
|---|---|
| `name` | what the game calls it |
| `textures` | `{"all": x}`, or `{"top", "bottom", "side"}`, or each of `top`, `bottom`, `front`, `back`, `left`, `right`: PNGs in `Resources/Textures/Blocks` |
| `hardness` | seconds to break it by hand |
| `tool` | the tool that's faster on it: `pickaxe`, `axe`, `shovel`, `sword` or `none` |
| `sound` | `soft`, `hard`, `sand`, `wood` or `glass` |
| `light` | light it gives off, 0 to 15 |
| `map` | its colour on the minimap, `#rrggbb` |

The server reads it too (hardness and tools decide how long breaking takes), so a
server and its players should share the same file. How blocks behave (solid, plant,
liquid, machine) stays in the code. `VoxelEngine --dump-block-data FILE` writes the
game's own details out as a fresh file.

**Creature models** (`creatures/<id>.json`): one for each creature, by id (`deer`, `wolf`, `bat`,
`dwarf`, `crossbow_dwarf`, `dwarf_chief`, `dwarf_miner`): a list of boxes, drawn facing
+x with its feet at 0. A part is a box (`box`: its centre, `size`, `color`: `#rrggbb`
or a name from the model's `colors`, `glow`) and/or a frame for the parts inside it
(`at`: its origin, a limb's pivot; `rotate`: degrees about x, y and z; `swing`: swung
about an axis as it walks; `flap`: flapped all the time; `when: "tame"`: only on a
tame wolf; `parts`: the parts inside it). A box can wear a block texture, tinted by its
colour: `"texture": "planks"` for every face, or `{"top", "bottom", "side"}`; with
`"tile": true` it repeats once a block, otherwise each face shows it once, stretched. A part can have a `name` (a label), and a model `animations`: keyframes that turn
and move labelled parts. `attack` plays as a creature strikes or shoots, `work` as a
miner digs, and `idle` (with `"loop": true`) all the time; the dwarves' are in
`dwarf_base`, swinging the `right_arm` and the `weapon`. A model may set a `scale`, and `inherits`
another's colours and parts, adding its own: the dwarves all start from `dwarf_base`
and change their colours and what they hold. The file explains itself at the top.

**Structure models** (`structures/<id>.json`): the multiblock structures' models (see
[Multiblock structures](#multiblock-structures)), in the same format. Models of blocks
drawn specially (the water wheel, shafts, gauges, pipes) are still in the code.

### The model editor

`ModelEditor` (built beside the game) is a program for these files, so they needn't be
edited by hand:

```bash
cd build && ./ModelEditor            # or: ./ModelEditor structures blast_furnace
```

- **Left**: creatures or structures, the models in the file (new, copy, rename,
  delete), what a model inherits, its scale, and its named colours (changing one
  changes every part using it).
- **Middle**: the model as the game draws it, with the part picked flashing, over a
  grid of whole blocks (a structure's 3 x 3 footprint in yellow; red is +x, the way a
  creature faces). Drag to turn round it, shift-drag or middle-drag to move, scroll to
  zoom. The View menu sets it walking or working and the light.
- **Blocks** (the third tab) lists every block, with a search, and shows the one picked
  as a cube. Its name, textures (one for every face; top, bottom and sides; or each
  face), hardness, best tool, sound, light and map colour can be changed. Blocks
  themselves are made in the code, so none can be added here.
- **Animations** (a tab beside the parts) picks one of the model's animations (or
  makes one), sets its length and whether it loops, and plays it in the view, or holds
  it at a time on the slider. Each labelled part (its own or inherited) has its keys:
  "Add a key here" adds one at the slider's time, posed as the part is then, to change
  its turn and move from. An inherited animation is copied into the model to change it.
- **Click a part** in the view to pick it, and drag the handles on it: **1** moves its
  box, **2** moves its frame (the pivot, with everything inside it), **3** sizes the box
  by the face you drag (Shift: both ways). Hold Ctrl to snap to sixteenths of a block.
  A part a model inherits is changed in the model it comes from.
- **Right**: the parts as a tree (add, add inside, copy, move up and down, delete),
  and the picked part's frame, box, colour, texture (picked from the block textures,
  one for every face or top, bottom and sides, repeated or stretched), glow, when it's
  shown, and how it swings or flaps. A label names a part in the tree (the game
  ignores it).

**Copy and paste** parts (Ctrl C, Ctrl V: in any model, in either file), and
**Mirror** one (Ctrl M, or the button): a copy on the other side, its places' z and its
turns about x and y turned round, its swing reversed, and left and right swapped in its
label, so a left arm makes the right one. **History** (Ctrl H, or the View menu) lists
every step since the file was opened; click one to go back (or forward) to just after
it.

Ctrl (Cmd) S saves to both `Resources/Data` in the source tree and the copy beside the
game, keeping the comments at the top; a running game picks the change up within a second.
Ctrl Z and Ctrl Shift Z undo and redo, and Revert reads the file again.

`DataTest` checks the JSON reader (comments, trailing commas, an error's line), that a
blocks file overrides just what it gives and loading another puts the rest back, and
that the shipped files read cleanly and `blocks.json` matches the game's own details,
and that the model files written out by the editor's writer read back the same.

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
- **Hurt** grunts, splashes into water, a **thump** when a creature is hit, a dwarf's
  grunt now and then, a chime when crafting, and **rain** while it rains where you
  are, muffled under cover (down to a quarter as loud with no open sky round you).
- **Machines** at work hum where they stand: water wheels creak and splash (louder the
  faster they turn), compressors chug, drills grind, pumps gurgle, engines putter,
  turbines whoosh, smelters and furnaces roar, crushers crunch, boilers bubble, air
  diffusers hiss, and formed structures each have their own (a derrick's pumpjack, a
  refinery's hum). The nearest eight within earshot play, as seamless loops started at
  different points so a row of one kind doesn't pulse in step. Pistons thunk as they
  push and pull.

The sounds were generated with Scenario's ElevenLabs Sound Effects model and converted
by `tools/convert_sounds.py`, which trims, shortens, fades and levels them into
`Resources/Sounds/` (16-bit mono WAV, with variants as `name_1.wav`, `name_2.wav` and so
on, picked at random at a slightly varied pitch). `tools/sound_prompts.txt` has each
sound's prompt and settings, to remake or change any of them.

On macOS the game uses the system's OpenAL framework. Elsewhere it uses an
installed OpenAL, such as openal-soft. Without one, or without an audio device, it
plays silently. The volume is in the settings. `VOXEL_MUTE=1` silences it, which is
handy for unattended screenshots. The code is in `src/audio/Audio.cpp`.

`SoundTest` checks, without a speaker:

- that every sound the game plays is there and loads;
- that blocks sound like their material;
- that a player gets events for another player's edits, and not their own.


Outdoors, under the open sky and not in heavy rain, you hear birdsong now and then by
day and crickets at night, from somewhere around you.
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

The first time you open the game it makes up a name for you (two words and a number,
such as "BraveOtter42"), which you can change in the settings. A server won't let in a
second player with a name someone there is already using (whatever its capitals): it
says so, and you can pick another.

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
- `VOXEL_HOLD_MINE=1` mines whatever's in view;
- `VOXEL_AUTO_FLY=1` flies straight ahead, sprinting (creative): with `VOXEL_BENCH=1`,
  a way to watch the world stream in as you travel. The bench output then also names
  the worst frame of each second over 25 ms, and where its time went.

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
many chunks were drawn. The server prints a line of its own: ticks, their average and
worst time, where it goes (traps, creatures, other updates, air power, signals, world
and sending), how many machines, air networks and signal blocks there are, and the
water simulation's ticks (it runs on its own thread), their time and its active cells.

Water by a river costs the same in the mountains as in the lowlands: about 9 ms a
water tick (four a second, on its own thread) with 5,000 to 8,500 cells moving, the
river flowing from spring to sinkhole the whole time.

Measured on this Mac (Release, normal world, by the river near spawn): about 108 fps,
the server's tick 0.08 ms (of the 50 it has), and the water simulation 4 ticks a second
of about 9 ms each with some 4,600 cells moving (the rivers never stop). With a large
factory beside it (1,024 air pipes, 1,024 item pipes and 32 each of compressors,
smelters, chests, air tanks, extractors, crafters, pistons and gauges, all on one
network) the tick averages 0.2 ms, the worst 2.5 ms (the air update, four times a
second), and frames lose about 0.3 ms to drawing. Pressure gauges are the only
machines with a real drawing cost (a dial is a dozen small boxes), so they're drawn in
less detail further off: the marks only within 12 blocks, only the housing from behind,
and none beyond 48.

The minimap **remembers** every column it has drawn (its colour, by 16 x 16 tile), so
a map centred somewhere new fills in at once from what's been seen, while the rows are
redrawn from the world as before; it's kept with the world (`map_cache.bin` in a world's
folder; a server's under `maps/`) and saved every 30 seconds, so a world you come back
to shows its map straight away.

The minimap used to cost 2 ms a frame (a fifth of the frame at 100 fps): it redrew six
rows of its picture every frame, looking up every block down each column, and again
down the column north of it for the shading. Now it redraws at a pace (the whole map
every 3 seconds, every 0.75 just after it's re-centred), takes the shading from the
row it drew before, and looks each chunk up once per column instead of every block:
0.1 ms, and about 15% more frames.

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
            Breathing, Creatures, Crafting, Hunger, Armour, Power, Signals, Smelting, Fluids,
            Dwarves, BlockData, Json, Multiblocks
tests/      NetSmokeTest, SaveTest, WaterAreaTest, PumpTest, EvaporationTest, WeatherTest,
            ChatTest, DungeonTest, TrapTest, ShapeTest, BiomeTest, ModeTest, MiningTest,
            BreathTest, CreatureTest, CraftTest, SoundTest, FoodTest, ChestTest, BedTest,
            PowerTest, SignalTest, FactoryTest, DataTest
tools/      convert_sounds.py and sound_prompts.txt (sound effects), make_item_textures.py (item icons)
```
