#include "world/Caves.h"

#include <algorithm>
#include <cmath>

#include "world/Noise.h"

namespace Caves {

namespace {

constexpr int S = Chunk::CHUNK_SIZE;
constexpr int L = S / kSpacing + 1; // lattice points per chunk axis

constexpr uint32_t kTunnelSeedA = 0x5EED0001u;
constexpr uint32_t kTunnelSeedB = 0x5EED0002u;
constexpr uint32_t kCavernSeed = 0x5EED0003u;

constexpr float kTunnelRadius = 0.075f; // in noise units; tunnels end up ~3-5 blocks wide
constexpr int kSurfaceCover = 10;       // tunnels taper away within this many blocks of the surface,
                                        // except in occasional entrance zones (IsEntranceZone)
constexpr int kCavernMinDepth = 24;     // caverns only this far below the surface
constexpr float kCavernThreshold = 0.42f;

struct Sample {
    float tunnelA, tunnelB, cavern;
};

Sample SampleAt(int wx, int wy, int wz) {
    // Tunnels are stretched horizontally so they wander more than they climb.
    return {
        Noise::Perlin3(wx / 56.0f, wy / 36.0f, wz / 56.0f, kTunnelSeedA),
        Noise::Perlin3(wx / 56.0f, wy / 36.0f, wz / 56.0f, kTunnelSeedB),
        Noise::Perlin3(wx / 90.0f, wy / 45.0f, wz / 90.0f, kCavernSeed),
    };
}

Sample Lerp(const Sample& a, const Sample& b, float t) {
    return { a.tunnelA + (b.tunnelA - a.tunnelA) * t, a.tunnelB + (b.tunnelB - a.tunnelB) * t, a.cavern + (b.cavern - a.cavern) * t };
}

// Trilinear interpolation of the 8 lattice samples around a point. c is indexed
// [dx + 2 * (dy + 2 * dz)]; f is the position within the lattice cell, 0..1.
Sample Interpolate(const Sample c[8], float fx, float fy, float fz) {
    Sample x00 = Lerp(c[0], c[1], fx), x10 = Lerp(c[2], c[3], fx);
    Sample x01 = Lerp(c[4], c[5], fx), x11 = Lerp(c[6], c[7], fx);
    return Lerp(Lerp(x00, x10, fy), Lerp(x01, x11, fy), fz);
}

// Whether tunnels may break through the surface in this column.
bool IsEntranceZone(int wx, int wz) {
    return Noise::Perlin2(wx / 64.0f - 4000.0f, wz / 64.0f + 9000.0f) > 0.32f;
}

bool IsCaveSample(const Sample& s, int depth, bool entranceZone) {
    if (depth < 0) return false; // above the ground: nothing to carve
    // Near the surface tunnels narrow to nothing, so they stay buried instead of
    // opening long trenches, unless this is an entrance zone.
    float radius = kTunnelRadius;
    if (!entranceZone && depth < kSurfaceCover) {
        float t = std::clamp((depth - 2.0f) / (kSurfaceCover - 2.0f), 0.0f, 1.0f);
        radius *= t * t * (3.0f - 2.0f * t);
    }
    if (s.tunnelA * s.tunnelA + s.tunnelB * s.tunnelB < radius * radius) return true;
    // Caverns fade in with depth so they don't break through to the surface.
    if (depth >= kCavernMinDepth) {
        float fadeIn = std::min(1.0f, (depth - kCavernMinDepth) / 16.0f);
        if (s.cavern > kCavernThreshold + (1.0f - fadeIn) * 0.2f) return true;
    }
    return false;
}

int FloorDiv(int a, int b) { int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) --q; return q; }

} // namespace

bool IsCave(int wx, int wy, int wz, int surfaceRow) {
    if (wy > surfaceRow) return false;
    const int lx = FloorDiv(wx, kSpacing), ly = FloorDiv(wy, kSpacing), lz = FloorDiv(wz, kSpacing);
    Sample c[8];
    for (int i = 0; i < 8; i++)
        c[i] = SampleAt((lx + (i & 1)) * kSpacing, (ly + ((i >> 1) & 1)) * kSpacing, (lz + (i >> 2)) * kSpacing);
    const float fx = static_cast<float>(wx - lx * kSpacing) / kSpacing;
    const float fy = static_cast<float>(wy - ly * kSpacing) / kSpacing;
    const float fz = static_cast<float>(wz - lz * kSpacing) / kSpacing;
    return IsCaveSample(Interpolate(c, fx, fy, fz), surfaceRow - wy, IsEntranceZone(wx, wz));
}

void Carve(const glm::ivec3& coords, const std::array<int, S * S>& rows, std::array<Block, Chunk::VOLUME>& blocks) {
    const int bottom = coords.y * S;
    // Nothing to carve if the whole chunk is above the ground.
    if (bottom > *std::max_element(rows.begin(), rows.end())) return;

    const glm::ivec3 origin = coords * S;
    Sample lattice[L][L][L];
    for (int i = 0; i < L; i++)
        for (int j = 0; j < L; j++)
            for (int k = 0; k < L; k++)
                lattice[i][j][k] = SampleAt(origin.x + i * kSpacing, origin.y + j * kSpacing, origin.z + k * kSpacing);

    for (int x = 0; x < S; x++) {
        const int i = x / kSpacing;
        const float fx = static_cast<float>(x % kSpacing) / kSpacing;
        for (int z = 0; z < S; z++) {
            const int k = z / kSpacing;
            const float fz = static_cast<float>(z % kSpacing) / kSpacing;
            const int surfaceRow = rows[x * S + z];
            const bool entranceZone = IsEntranceZone(origin.x + x, origin.z + z);
            for (int y = 0; y < S; y++) {
                const int wy = bottom + y;
                if (wy > surfaceRow) break;
                Block& block = blocks[x + S * (y + S * z)];
                if (block.IsAir()) continue;

                const int j = y / kSpacing;
                const float fy = static_cast<float>(y % kSpacing) / kSpacing;
                const Sample c[8] = {
                    lattice[i][j][k], lattice[i + 1][j][k], lattice[i][j + 1][k], lattice[i + 1][j + 1][k],
                    lattice[i][j][k + 1], lattice[i + 1][j][k + 1], lattice[i][j + 1][k + 1], lattice[i + 1][j + 1][k + 1],
                };
                if (IsCaveSample(Interpolate(c, fx, fy, fz), surfaceRow - wy, entranceZone)) block = Block();
            }
        }
    }
}

} // namespace Caves
