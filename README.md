# VoxelEngine

A small OpenGL voxel engine with sloped terrain, chunk streaming on a background
thread, block lighting, ambient occlusion, a skybox and a black hole lensing effect.

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
| Space | Jump (fly: up) |
| Shift | Sprint (fly: faster) |
| F | Toggle fly mode (no collision) |
| Q / E | Fly down / up |
| Mouse | Look |
| Left click | Break block |
| Right click | Place the selected block |
| 1-4 | Select block: dirt, stone, grass, lamp |
| L (hold) | Laser: carve a tunnel along the view ray |
| R | Reload shaders and block textures, rebuild all chunk meshes |
| Tab | Toggle mouse capture |
| F1 | Toggle wireframe |
| F2 | Toggle debug window |
| F3 | Toggle ambient occlusion |
| N | Toggle day / night |
| Esc | Quit |

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
           Lighting, Collision, Raycast
```
