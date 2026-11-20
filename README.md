# VoxelEngine

A small OpenGL voxel engine with sloped terrain, caves, trees, chunk streaming on a
background thread, block lighting, ambient occlusion, finite flowing water, a skybox
and a black hole lensing effect. Several players can share a world over the network.

## Building

Dependencies come from [vcpkg](https://vcpkg.io) (manifest mode). With `VCPKG_ROOT` set:

```bash
cmake --preset default
cmake --build build
cd build && ./VoxelEngine
```

The executable loads `Resources/` relative to the working directory, and the
build copies it next to the binary.

The build also produces `VoxelServer` (a dedicated server that doesn't link GL or
GLFW), `NetSmokeTest` (see [Multiplayer](#multiplayer)), `SaveTest` (see
[Saving](#saving)) and `WaterAreaTest` (see [Water](#water)).

## Multiplayer

Every game is a client of a server. Started without arguments, the game runs a
server inside the same process on a free port of the loopback interface and
connects to it, so singleplayer and multiplayer use the same code.

Host a dedicated server (no window; Ctrl+C stops it):

```bash
cd build && ./VoxelEngine --server
```

`./VoxelEngine --server 30000` picks another port (the default is UDP 27450), and
`./VoxelServer [port]` does the same without any graphics libraries. Others join
with:

```bash
cd build && ./VoxelEngine --connect 192.168.1.20 --name Alice
```

`--connect host:port` sets the port. To play on the same machine as the server,
connect to `127.0.0.1`. Over the internet, forward UDP port 27450 to the server.
Other players appear as box figures in their own colour, with a dark visor on the
side they're facing. The server saves the world (see [Saving](#saving)).

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

The server saves the world to a directory: `world/` for a dedicated server,
`saves/singleplayer/` for singleplayer (both relative to the working directory).
`--world DIR` picks another one, and `--no-save` neither loads nor saves anything.
Works the same with `VoxelEngine` and `VoxelServer`:

```bash
cd build && ./VoxelServer --world worlds/survival
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
- **Format** (`src/server/WorldStorage.h`): `world.dat` holds the format version
  and the players; `regions/r.X.Y.Z.vxr` holds the stored chunks of an 8x8x8-chunk
  region, as the same run-length encoded block planes the network uses (an edited
  surface chunk is a few KB). Each file is written to a temporary file and renamed,
  so a crash mid-save leaves the previous save intact. A damaged file, or one from
  a newer format, stops the server from starting rather than being skipped and
  then overwritten.

`SaveTest` saves and reloads a world in a temporary directory. It checks that
edits come back, that chunks are saved when they unload, light under a saved roof,
that damaged files are refused, and that a rejoining player is put back in place.

## Controls

| Key | Action |
| --- | --- |
| WASD | Walk (or fly) |
| Space | Jump (swim: up; fly: up) |
| Shift | Sprint (fly: faster) |
| F | Toggle fly mode (no collision) |
| Q / E | Fly down / up |
| Mouse | Look |
| Left click | Break block |
| Right click | Place the selected block |
| 1-8 / mouse wheel | Select the block to place (shown in the hotbar at the bottom; with Tab you can also click a slot) |
| L (hold) | Laser: carve a tunnel along the view ray |
| R | Reload shaders and block textures, rebuild all chunk meshes |
| Tab | Toggle mouse capture |
| F1 | Toggle wireframe |
| F2 | Toggle debug window |
| F3 | Toggle ambient occlusion |
| F12 | Save a screenshot (`screenshot-<date>-<time>.png` in the working directory) |
| N | Toggle day / night |
| Esc | Quit |

## World generation

`TerrainGenerator` builds each chunk independently and deterministically:

1. **Surface:** a heightmap from 8 octaves of Perlin noise, snapped to eighths of
   a block. Each column gets a sloped grass block on top, then dirt, then stone.
2. **Caves** (`Caves.cpp`): winding tunnels where two 3D noise fields are both
   near zero, plus large caverns more than 24 blocks down. Tunnels that reach
   the surface form entrances. Noise is sampled every 4 blocks and interpolated.
3. **Features** (`Features.cpp`): trees cluster in forests, plus bushes and
   boulders. Each kind sits on a jittered grid, so a chunk can generate the parts
   of features rooted in neighbouring columns.
4. **Plants:** tall grass and flowers, drawn as crossed sprites that sit on the
   sloped surface. Breaking the block under a plant removes it.

Open space below sea level (row -3) becomes seas and lakes, with sandy beaches
and sea floors. Caves never open under water, so they stay dry.

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
waterlogged. Water is only ever moved, never created or destroyed. It falls into
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

The simulation runs on the server, on its own thread, up to 32,768 water cells per tick, ten
ticks a second, and only near something that changed. It holds a world-wide edit
lock in short slices and steps aside for waiting edits, so player edits never
wait more than a couple of milliseconds. Place a full block of water with 8.

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

## Lighting

Two light channels, each 0-15, are flood-filled on the server's world worker thread and
cross chunk borders:

- **Sky light** comes down from above at full strength and loses one level per
  block sideways, so overhangs, tunnels and caves get darker the further they
  are from open sky. The day/night setting scales it in the shader.
- **Block light** spreads from lamps (level 15), losing one level per block.
- Both lose two levels instead of one entering water.

Leaves and plants let light through, so the ground under trees stays lit.

Faces combine both with a directional sun term and per-vertex ambient occlusion,
all smoothed across vertices.

## Block textures

Every PNG in `Resources/Textures/Blocks/` is packed into a texture atlas at
startup (and again when you press R). Textures are referenced by file name
without the extension; the face-to-texture mapping for each block type lives in
`kBlockTextureNames` in `src/world/Block.cpp`. Tiles should all be the same
square size (others are resampled to match the first). Missing textures show up
as a magenta checkerboard.

## Layout

```
src/
  main.cpp  command line: singleplayer, --server, --connect, --world
  app/      Application (window, input, main loop), Camera, Player (movement and physics)
  client/   Client (connection), ClientWorld (received chunks, mesh worker),
            ClientChunk (meshes and GL buffers), ChunkMesher
  server/   Server (clients, streaming, validation), ServerWorld (chunk loading,
            generation and light worker), Water, WorldStorage (saving),
            main.cpp (VoxelServer)
  net/      Protocol (messages, RLE chunk planes), Transport (ENet wrapper)
  render/   Renderer (frame passes, skybox, post effect), PlayerModels, Shader,
            Textures, DebugDraw
  world/    Shared by both sides: Chunk, Block, BlockAccess, TerrainGenerator, Caves,
            Features, Noise, Lighting, Collision, Raycast
tests/      NetSmokeTest, SaveTest, WaterAreaTest
```
