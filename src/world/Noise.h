#pragma once

#include <cstdint>

// Deterministic hashing and gradient noise for world generation. Thread-safe.
namespace Noise {

uint32_t Hash(int x, int y, int z, uint32_t seed);
// Uniform in [0, 1).
float Random01(int x, int y, int z, uint32_t seed);

// 2D Perlin noise (the terrain heightmap's base noise), roughly in [-0.7, 0.7].
float Perlin2(float x, float y);
// 3D Perlin noise, roughly in [-1, 1].
float Perlin3(float x, float y, float z, uint32_t seed);

} // namespace Noise
