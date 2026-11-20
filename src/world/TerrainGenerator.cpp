#include "world/TerrainGenerator.h"

#include <cmath>

namespace {

struct Vec2 {
    float x, y;
};

// Hash-based gradient so no permutation table is needed and any grid coordinate works.
Vec2 RandomGradient(int ix, int iy) {
    const unsigned w = 8 * sizeof(unsigned);
    const unsigned s = w / 2;
    unsigned a = static_cast<unsigned>(ix);
    unsigned b = static_cast<unsigned>(iy);
    a *= 3284157443u;

    b ^= (a << s) | a >> (w - s);
    b *= 1911520717u;

    a ^= (b << s) | b >> (w - s);
    a *= 2048419325u;
    float angle = a * (3.14159265f / ~(~0u >> 1)); // in [0, 2*Pi]

    return { std::sin(angle), std::cos(angle) };
}

float DotGridGradient(int ix, int iy, float x, float y) {
    Vec2 gradient = RandomGradient(ix, iy);
    float dx = x - static_cast<float>(ix);
    float dy = y - static_cast<float>(iy);
    return dx * gradient.x + dy * gradient.y;
}

// Cubic (smoothstep) interpolation.
float Interpolate(float a0, float a1, float w) {
    return (a1 - a0) * (3.0f - w * 2.0f) * w * w + a0;
}

float Perlin(float x, float y) {
    // floor, not truncation: (int)-0.5 == 0 would give weights outside [0, 1]
    // and produce visible seams/mirroring for negative coordinates.
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    float sx = x - static_cast<float>(x0);
    float sy = y - static_cast<float>(y0);

    float ix0 = Interpolate(DotGridGradient(x0, y0, x, y), DotGridGradient(x1, y0, x, y), sx);
    float ix1 = Interpolate(DotGridGradient(x0, y1, x, y), DotGridGradient(x1, y1, x, y), sx);
    return Interpolate(ix0, ix1, sy);
}

} // namespace

float TerrainGenerator::GetHeight(int x, int z) const {
    float value = 0.0f;
    float freq = 1.0f;
    float amp = 1.0f;
    float ampTotal = 0.0f;

    for (int i = 0; i < OCTAVES; i++) {
        value += Perlin(x * freq / SIZE, z * freq / SIZE) * amp;
        ampTotal += amp;
        freq *= 2.0f;
        amp *= 0.5f;
    }

    return value / ampTotal * AMPLITUDE;
}
