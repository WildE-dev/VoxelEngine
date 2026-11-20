# VoxelEngine

A small OpenGL voxel engine with sloped terrain, caves, trees, chunk streaming on a
background thread, block lighting, ambient occlusion, a skybox and a black hole
lensing effect.

## Building

Dependencies come from [vcpkg](https://vcpkg.io) (manifest mode). With `VCPKG_ROOT` set:

```bash
cmake --preset default
cmake --build build
cd build && ./VoxelEngine
```

The executable loads `Resources/` relative to the working directory, and the
build copies it next to the binary.

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

Water is translucent and drawn in its own pass after solid geometry. It absorbs
light, so deeper water gets darker, and the screen tints blue when your head is
underwater. You swim in it: you sink slowly, Space swims up, and swimming into
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

The simulation runs on its own thread, up to 32,768 water cells per tick, ten
ticks a second, and only near something that changed. It holds a world-wide edit
lock in short slices and steps aside for waiting edits, so player edits never
wait more than a couple of milliseconds. Place a full block of water with 8.

## Player

You walk on the terrain with gravity and collision. The body is a 0.6 x 1.8
box. Full blocks collide as boxes. Sloped terrain blocks collide as a height
field that matches their rendered triangles, so you stand on the visible
surface and walk up and down hills smoothly. You can step up ledges up to 0.6
blocks; anything taller needs a jump (about 1.25 blocks high). Physics runs at
a fixed 120 Hz. Until the chunks under you have loaded, you wait in place. Fly
mode (F) moves freely through blocks like the old free camera.

## Lighting

Two light channels, each 0-15, are flood-filled on the world worker thread and
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
  main.cpp
  app/     Application (window, input, main loop), Camera, Player (movement and physics)
  render/  Renderer (frame passes, skybox, post effect), Shader, Textures, DebugDraw
  world/   World (chunk streaming + worker thread), Chunk, Block, TerrainGenerator,
           Caves, Features, Noise, Lighting, Water, Collision, Raycast
```
