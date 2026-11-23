#include "world/Noise.h"

#include <cmath>

namespace Noise {

namespace {

// --- 2D noise (unchanged from the original terrain generator so terrain stays the same) ---

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

// --- 3D noise ---

float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
float Lerp(float a, float b, float t) { return a + (b - a) * t; }

float Grad3(uint32_t h, float x, float y, float z) {
    switch (h % 12) {
    case 0:  return  x + y;
    case 1:  return -x + y;
    case 2:  return  x - y;
    case 3:  return -x - y;
    case 4:  return  x + z;
    case 5:  return -x + z;
    case 6:  return  x - z;
    case 7:  return -x - z;
    case 8:  return  y + z;
    case 9:  return -y + z;
    case 10: return  y - z;
    default: return -y - z;
    }
}

} // namespace

uint32_t Hash(int x, int y, int z, uint32_t seed) {
    uint32_t h = seed * 0x9E3779B9u;
    h ^= static_cast<uint32_t>(x) * 0x8DA6B343u;
    h ^= static_cast<uint32_t>(y) * 0xD8163841u;
    h ^= static_cast<uint32_t>(z) * 0xCB1AB31Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

float Random01(int x, int y, int z, uint32_t seed) {
    return (Hash(x, y, z, seed) >> 8) * (1.0f / 16777216.0f);
}

float Perlin2(float x, float y) {
    // floor, not truncation: (int)-0.5 == 0 would give weights outside [0, 1].
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

float Perlin3(float x, float y, float z, uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int z0 = static_cast<int>(std::floor(z));
    const float fx = x - x0, fy = y - y0, fz = z - z0;
    const float u = Fade(fx), v = Fade(fy), w = Fade(fz);

    auto g = [&](int dx, int dy, int dz) {
        return Grad3(Hash(x0 + dx, y0 + dy, z0 + dz, seed), fx - dx, fy - dy, fz - dz);
    };

    return Lerp(Lerp(Lerp(g(0, 0, 0), g(1, 0, 0), u), Lerp(g(0, 1, 0), g(1, 1, 0), u), v),
                Lerp(Lerp(g(0, 0, 1), g(1, 0, 1), u), Lerp(g(0, 1, 1), g(1, 1, 1), u), v), w);
}

} // namespace Noise
