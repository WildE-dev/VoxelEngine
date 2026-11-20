# VoxelEngine

A small OpenGL voxel engine with sloped terrain, chunk streaming on a background
thread, a skybox and a black hole lensing effect.

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
| WASD / Q E | Move / down, up |
| Shift | Move faster |
| Mouse | Look |
| Left click | Break block |
| Right click | Place dirt |
| L (hold) | Laser: carve a tunnel along the view ray |
| R | Reload shaders and rebuild all chunk meshes |
| Tab | Toggle mouse capture |
| F1 | Toggle wireframe |
| F2 | Toggle debug window |
| Esc | Quit |

## Layout

```
src/
  main.cpp
  app/     Application (window, input, main loop), Camera
  render/  Renderer (frame passes, skybox, post effect), Shader, Textures, DebugDraw
  world/   World (chunk streaming + worker thread), Chunk, Block, TerrainGenerator, Raycast
```
