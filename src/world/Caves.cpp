#include "world/Caves.h"

#include <algorithm>
#include <cmath>

#include "world/Noise.h"
#include "world/TerrainGenerator.h"

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

// Whether tunnels may break through the surface in this column. Never on beaches or
// under water.
bool IsEntranceZone(int wx, int wz, int surfaceRow) {
    if (surfaceRow <= TerrainGenerator::kSeaLevel + 1) return false; // beaches, sea floors and just above
    return Noise::Perlin2(wx / 64.0f - 4000.0f, wz / 64.0f + 9000.0f) > 0.32f;
}

// How open a sample is: above 0 carved out. (The tunnel's and the cavern's tests, each as
// a value crossing 0 where the test turns; the larger of the two.)
float OpennessOf(const Sample& s, int depth, bool entranceZone, int wy) {
    if (depth < 0) return -1.0f; // above the ground: nothing to carve
    // Below the lava level they thin out and grow rarer, to a third of their size.
    const float deep = std::clamp(static_cast<float>(TerrainGenerator::kDeepTop - wy) / TerrainGenerator::kCavesThinOut, 0.0f, 1.0f);
    // Near the surface tunnels narrow to nothing, so they stay buried instead of
    // opening long trenches, unless this is an entrance zone.
    float radius = kTunnelRadius * (1.0f - 0.65f * deep);
    if (!entranceZone && depth < kSurfaceCover) {
        float t = std::clamp((depth - 2.0f) / (kSurfaceCover - 2.0f), 0.0f, 1.0f);
        radius *= t * t * (3.0f - 2.0f * t);
    }
    float open = radius * radius - (s.tunnelA * s.tunnelA + s.tunnelB * s.tunnelB);
    // Caverns fade in with depth so they don't break through to the surface.
    if (depth >= kCavernMinDepth) {
        float fadeIn = std::min(1.0f, (depth - kCavernMinDepth) / 16.0f);
        open = std::max(open, (s.cavern - (kCavernThreshold + (1.0f - fadeIn) * 0.2f + 0.15f * deep)) * 0.05f);
    }
    return open;
}

bool IsCaveSample(const Sample& s, int depth, bool entranceZone, int wy) { return OpennessOf(s, depth, entranceZone, wy) > 0.0f; }

int FloorDiv(int a, int b) { int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) --q; return q; }

} // namespace

namespace {
float OpennessAt(int wx, int wy, int wz, int surfaceRow) {
    if (wy > surfaceRow) return -1.0f;
    const int lx = FloorDiv(wx, kSpacing), ly = FloorDiv(wy, kSpacing), lz = FloorDiv(wz, kSpacing);
    Sample c[8];
    for (int i = 0; i < 8; i++)
        c[i] = SampleAt((lx + (i & 1)) * kSpacing, (ly + ((i >> 1) & 1)) * kSpacing, (lz + (i >> 2)) * kSpacing);
    const float fx = static_cast<float>(wx - lx * kSpacing) / kSpacing;
    const float fy = static_cast<float>(wy - ly * kSpacing) / kSpacing;
    const float fz = static_cast<float>(wz - lz * kSpacing) / kSpacing;
    return OpennessOf(Interpolate(c, fx, fy, fz), surfaceRow - wy, IsEntranceZone(wx, wz, surfaceRow), wy);
}
} // namespace

bool IsCave(int wx, int wy, int wz, int surfaceRow) { return OpennessAt(wx, wy, wz, surfaceRow) > 0.0f; }

Field::Field(const glm::ivec3& chunkCoords) {
    // From a lattice point at or before the block just before the chunk, to one at or
    // after the block just after it.
    const glm::ivec3 origin = chunkCoords * S;
    base = glm::ivec3(FloorDiv(origin.x - 1, kSpacing), FloorDiv(origin.y - 1, kSpacing), FloorDiv(origin.z - 1, kSpacing)) * kSpacing;
    for (int i = 0; i < kPoints; i++)
        for (int j = 0; j < kPoints; j++)
            for (int k = 0; k < kPoints; k++) {
                const Sample s = SampleAt(base.x + i * kSpacing, base.y + j * kSpacing, base.z + k * kSpacing);
                float* p = &lattice[((i * kPoints + j) * kPoints + k) * 3];
                p[0] = s.tunnelA, p[1] = s.tunnelB, p[2] = s.cavern;
            }
}

float Field::Openness(int wx, int wy, int wz, int surfaceRow) const {
    if (wy > surfaceRow) return -1.0f;
    const int i = FloorDiv(wx - base.x, kSpacing), j = FloorDiv(wy - base.y, kSpacing), k = FloorDiv(wz - base.z, kSpacing);
    if (i < 0 || j < 0 || k < 0 || i + 1 >= kPoints || j + 1 >= kPoints || k + 1 >= kPoints) return OpennessAt(wx, wy, wz, surfaceRow); // (beyond it)
    auto at = [&](int a, int b, int c) {
        const float* p = &lattice[(((i + a) * kPoints + (j + b)) * kPoints + (k + c)) * 3];
        return Sample{ p[0], p[1], p[2] };
    };
    const Sample c[8] = { at(0, 0, 0), at(1, 0, 0), at(0, 1, 0), at(1, 1, 0), at(0, 0, 1), at(1, 0, 1), at(0, 1, 1), at(1, 1, 1) };
    const float fx = static_cast<float>(wx - base.x - i * kSpacing) / kSpacing;
    const float fy = static_cast<float>(wy - base.y - j * kSpacing) / kSpacing;
    const float fz = static_cast<float>(wz - base.z - k * kSpacing) / kSpacing;
    return OpennessOf(Interpolate(c, fx, fy, fz), surfaceRow - wy, IsEntranceZone(wx, wz, surfaceRow), wy);
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
            const bool entranceZone = IsEntranceZone(origin.x + x, origin.z + z, surfaceRow);
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
                if (IsCaveSample(Interpolate(c, fx, fy, fz), surfaceRow - wy, entranceZone, wy)) block = Block();
            }
        }
    }
}

} // namespace Caves
