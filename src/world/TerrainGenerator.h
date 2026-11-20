#pragma once

// Stateless heightmap generator built on fractal Perlin noise.
// Safe to call from any thread.
class TerrainGenerator {
public:
    float GetHeight(int x, int z) const;

private:
    static constexpr int OCTAVES = 8;
    static constexpr float SIZE = 120.0f;
    static constexpr float AMPLITUDE = 20.0f;
};
