#include "world/Features.h"

#include <algorithm>
#include <cmath>

#include "world/Caves.h"
#include "world/Noise.h"
#include "world/TerrainGenerator.h"

namespace Features {

namespace {

constexpr int S = Chunk::CHUNK_SIZE;

struct Grid {
    int size;     // grid cell size in blocks
    int margin;   // keep features this far inside their cell
    int reach;    // furthest a feature extends from its root column
    uint32_t seed;
};

constexpr Grid kTrees{ 8, 2, 2, 0x7EE50001u };
constexpr Grid kBushes{ 6, 1, 1, 0xB0540002u };
constexpr Grid kBoulders{ 16, 3, 2, 0xB01D0003u };

int FloorDiv(int a, int b) { int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) --q; return q; }

// Forests: a large-scale noise field that makes trees cluster.
float Forest(int wx, int wz) { return Noise::Perlin2(wx / 110.0f + 1000.0f, wz / 110.0f - 500.0f); }

struct Candidate {
    int wx, wz;
    uint32_t hash;
};

// The feature (if any) in a grid cell: its root column and a hash for its shape.
bool CandidateIn(const Grid& grid, int gx, int gz, float baseChance, float forestBonus, Candidate& out) {
    const uint32_t h = Noise::Hash(gx, 0, gz, grid.seed);
    const int span = grid.size - 2 * grid.margin;
    out.wx = gx * grid.size + grid.margin + static_cast<int>(h % span);
    out.wz = gz * grid.size + grid.margin + static_cast<int>((h >> 8) % span);
    out.hash = Noise::Hash(out.wx, 1, out.wz, grid.seed);
    const float chance = std::clamp(baseChance + Forest(out.wx, out.wz) * forestBonus, 0.0f, 0.95f);
    return Noise::Random01(gx, 2, gz, grid.seed) < chance;
}

constexpr float kTreeChance = 0.25f, kTreeForest = 1.6f;
constexpr float kBushChance = 0.10f, kBushForest = 0.4f;
constexpr float kBoulderChance = 0.22f, kBoulderForest = 0.0f;

// Calls fn for every candidate of a grid whose reach touches the chunk's columns.
template <typename Fn>
void ForEachCandidate(const Grid& grid, const glm::ivec3& coords, float chance, float forest, Fn fn) {
    const int x0 = coords.x * S - grid.reach, x1 = coords.x * S + S - 1 + grid.reach;
    const int z0 = coords.z * S - grid.reach, z1 = coords.z * S + S - 1 + grid.reach;
    for (int gx = FloorDiv(x0, grid.size); gx <= FloorDiv(x1, grid.size); gx++)
        for (int gz = FloorDiv(z0, grid.size); gz <= FloorDiv(z1, grid.size); gz++) {
            Candidate c;
            if (!CandidateIn(grid, gx, gz, chance, forest, c)) continue;
            if (c.wx < x0 || c.wx > x1 || c.wz < z0 || c.wz > z1) continue;
            fn(c);
        }
}

// Writes into the chunk if the world position falls inside it.
struct Writer {
    const glm::ivec3 origin;
    std::array<Block, Chunk::VOLUME>& blocks;

    Block* At(int wx, int wy, int wz) const {
        const int x = wx - origin.x, y = wy - origin.y, z = wz - origin.z;
        if (x < 0 || y < 0 || z < 0 || x >= S || y >= S || z >= S) return nullptr;
        return &blocks[x + S * (y + S * z)];
    }
    // Leaves only fill air and plants, never terrain or trunks.
    void Leaves(int wx, int wy, int wz) const {
        if (Block* b = At(wx, wy, wz); b && (b->IsAir() || b->IsPlant())) *b = Block(BlockType::LEAVES);
    }
    void Solid(int wx, int wy, int wz, BlockType type) const {
        if (Block* b = At(wx, wy, wz)) *b = Block(type);
    }
    // Leaves at ground level may also replace the sloped grass top, but never stone
    // or logs (that would leave whatever is above hanging over transparent leaves).
    void GroundLeaves(int wx, int wy, int wz) const {
        Block* b = At(wx, wy, wz);
        if (b && (b->IsPassable() || (b->type == BlockType::GRASS && !b->IsFullBlock()))) *b = Block(BlockType::LEAVES);
    }
};

// Root must be on intact ground: no cave entrance under or at the surface.
bool OnSolidGround(int wx, int wz, int surfaceRow) {
    return !Caves::IsCave(wx, surfaceRow, wz, surfaceRow) && !Caves::IsCave(wx, surfaceRow - 1, wz, surfaceRow);
}

void PlaceTree(const TerrainGenerator& terrain, const Candidate& c, const Writer& w) {
    const int base = terrain.ColumnAt(c.wx, c.wz).surfaceRow;
    if (!OnSolidGround(c.wx, c.wz, base)) return;

    // Trunk from the ground up: it replaces the sloped grass block it stands on.
    const int height = 4 + static_cast<int>(c.hash % 3);
    const int top = base + height - 1;
    for (int y = base; y <= top; y++) w.Solid(c.wx, y, c.wz, BlockType::LOG);

    // Canopy: two wide layers around the upper trunk, then two narrow ones on top.
    for (int y = top - 2; y <= top + 1; y++) {
        const int radius = y <= top - 1 ? 2 : 1;
        for (int dx = -radius; dx <= radius; dx++) {
            for (int dz = -radius; dz <= radius; dz++) {
                const bool corner = std::abs(dx) == radius && std::abs(dz) == radius;
                // Wide layers lose random corners; the very top is a plus shape.
                if (corner && (y == top + 1 || Noise::Random01(c.wx + dx, y, c.wz + dz, c.hash) < 0.6f)) continue;
                w.Leaves(c.wx + dx, y, c.wz + dz);
            }
        }
    }
}

void PlaceBush(const TerrainGenerator& terrain, const Candidate& c, const Writer& w) {
    const auto column = terrain.ColumnAt(c.wx, c.wz);
    if (!OnSolidGround(c.wx, c.wz, column.surfaceRow)) return;
    // A small leafy mound sitting on the ground (replacing the sloped grass top).
    const int base = column.surfaceRow;
    w.GroundLeaves(c.wx, base, c.wz);
    w.Leaves(c.wx, base + 1, c.wz);
    for (const auto& d : { glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1) }) {
        if (Noise::Random01(c.wx + d.x, base, c.wz + d.y, c.hash) < 0.65f) {
            const int ground = terrain.ColumnAt(c.wx + d.x, c.wz + d.y).surfaceRow;
            if (ground == base && OnSolidGround(c.wx + d.x, c.wz + d.y, ground))
                w.GroundLeaves(c.wx + d.x, ground, c.wz + d.y);
        }
    }
}

void PlaceBoulder(const TerrainGenerator& terrain, const Candidate& c, const Writer& w) {
    const int radius = 1 + static_cast<int>(c.hash % 2);
    const int centreGround = terrain.ColumnAt(c.wx, c.wz).surfaceRow;
    for (int dx = -radius; dx <= radius; dx++) {
        for (int dz = -radius; dz <= radius; dz++) {
            const float dist = std::sqrt(static_cast<float>(dx * dx + dz * dz));
            if (dist > radius + 0.3f) continue;
            const int wx = c.wx + dx, wz = c.wz + dz;
            const int ground = terrain.ColumnAt(wx, wz).surfaceRow;
            if (!OnSolidGround(wx, wz, ground)) continue;
            // Dome profile. Each column is filled solid from its own ground upward,
            // so the boulder never overhangs air.
            const int domeTop = centreGround + static_cast<int>(std::floor(std::sqrt(std::max(0.0f, (radius + 0.5f) * (radius + 0.5f) - dist * dist))));
            for (int y = ground; y <= domeTop; y++) w.Solid(wx, y, wz, BlockType::STONE);
        }
    }
}

bool ColumnNear(const Candidate& c, int wx, int wz, int reach) {
    return std::abs(c.wx - wx) <= reach && std::abs(c.wz - wz) <= reach;
}

} // namespace

void Place(const TerrainGenerator& terrain, const glm::ivec3& coords, const std::array<int, S * S>& rows,
           std::array<Block, Chunk::VOLUME>& blocks) {
    // Features sit on the surface; skip chunks far above or below it. Neighbouring
    // columns can differ a little from this chunk's, hence the margins.
    const auto [lo, hi] = std::minmax_element(rows.begin(), rows.end());
    const int bottom = coords.y * S, top = bottom + S - 1;
    if (bottom > *hi + 12 || top < *lo - 4) return;

    const Writer w{ coords * S, blocks };
    ForEachCandidate(kBoulders, coords, kBoulderChance, kBoulderForest, [&](const Candidate& c) { PlaceBoulder(terrain, c, w); });
    ForEachCandidate(kBushes, coords, kBushChance, kBushForest, [&](const Candidate& c) { PlaceBush(terrain, c, w); });
    ForEachCandidate(kTrees, coords, kTreeChance, kTreeForest, [&](const Candidate& c) { PlaceTree(terrain, c, w); });
}

bool MayOccupyColumn(int wx, int wz) {
    auto near = [&](const Grid& grid, float chance, float forest, int reach) {
        const int gx = FloorDiv(wx, grid.size), gz = FloorDiv(wz, grid.size);
        for (int dx = -1; dx <= 1; dx++)
            for (int dz = -1; dz <= 1; dz++) {
                Candidate c;
                if (CandidateIn(grid, gx + dx, gz + dz, chance, forest, c) && ColumnNear(c, wx, wz, reach)) return true;
            }
        return false;
    };
    return near(kTrees, kTreeChance, kTreeForest, 0) ||
           near(kBushes, kBushChance, kBushForest, 1) ||
           near(kBoulders, kBoulderChance, kBoulderForest, kBoulders.reach);
}

} // namespace Features
