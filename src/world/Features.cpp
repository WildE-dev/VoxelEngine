#include "world/Features.h"

#include <climits>
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

constexpr Grid kTrees{ 8, 2, 3, 0x7EE50001u };
constexpr Grid kBushes{ 6, 1, 1, 0xB0540002u };
constexpr Grid kBoulders{ 16, 3, 2, 0xB01D0003u };

enum class Kind { Tree, Bush, Boulder };

int FloorDiv(int a, int b) { int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) --q; return q; }

// Woods cluster: a large-scale field that thickens and thins them within a biome.
float Clumping(int wx, int wz) {
    return std::clamp(0.55f + 1.1f * Noise::Perlin2(wx / 110.0f + 1000.0f, wz / 110.0f - 500.0f), 0.15f, 1.6f);
}

// How likely a grid cell is to hold a feature of this kind, by the biome at its root.
float ChanceFor(Kind kind, Biome biome) {
    switch (kind) {
    case Kind::Tree:
        switch (biome) {
        case Biome::Plains: return 0.05f;
        case Biome::Forest: return 0.6f;
        case Biome::Swamp: return 0.3f;
        case Biome::Desert: return 0.16f; // cacti
        case Biome::Savanna: return 0.1f;
        case Biome::Jungle: return 0.85f;
        case Biome::Taiga: return 0.55f;
        case Biome::SnowyTaiga: return 0.35f;
        case Biome::Tundra: return 0.03f;
        default: return 0.0f;
        }
    case Kind::Bush:
        switch (biome) {
        case Biome::Plains: return 0.08f;
        case Biome::Forest: return 0.15f;
        case Biome::Swamp: return 0.2f;
        case Biome::Savanna: return 0.06f;
        case Biome::Jungle: return 0.35f;
        case Biome::Taiga: return 0.08f;
        case Biome::SnowyTaiga: return 0.02f;
        default: return 0.0f; // deserts, tundra
        }
    case Kind::Boulder:
        return biome == Biome::Swamp ? 0.04f : biome == Biome::Desert ? 0.1f : 0.22f;
    }
    return 0.0f;
}

struct Candidate {
    int wx, wz;
    uint32_t hash;
    Biome biome;
};

// The feature (if any) in a grid cell: its root column, a hash for its shape, and the
// biome there (which decides how likely it is and what it is).
bool CandidateIn(const TerrainGenerator& terrain, const Grid& grid, Kind kind, int gx, int gz, Candidate& out) {
    const uint32_t h = Noise::Hash(gx, 0, gz, grid.seed);
    const int span = grid.size - 2 * grid.margin;
    out.wx = gx * grid.size + grid.margin + static_cast<int>(h % span);
    out.wz = gz * grid.size + grid.margin + static_cast<int>((h >> 8) % span);
    out.hash = Noise::Hash(out.wx, 1, out.wz, grid.seed);
    const float roll = Noise::Random01(gx, 2, gz, grid.seed);
    if (roll >= 0.95f) return false; // never more likely than this: skip the biome lookup
    // Nothing grows in a mountain lake or a river, or right on their shores.
    for (const auto& d : { glm::ivec2(0, 0), glm::ivec2(3, 0), glm::ivec2(-3, 0), glm::ivec2(0, 3), glm::ivec2(0, -3) })
        if (terrain.LakeLevelAt(out.wx + d.x, out.wz + d.y) != INT_MIN) return false;
    if (terrain.InRiver(out.wx, out.wz, 3.0f)) return false;
    // (One height sample for the altitude: enough for the climate, and cheap.)
    out.biome = terrain.BiomeAt(out.wx, out.wz, static_cast<int>(std::floor(terrain.GetHeight(out.wx, out.wz))));
    float chance = ChanceFor(kind, out.biome);
    if (kind != Kind::Boulder) chance *= Clumping(out.wx, out.wz);
    return roll < std::min(chance, 0.95f);
}

// Calls fn for every candidate of a grid whose reach touches the chunk's columns.
template <typename Fn>
void ForEachCandidate(const TerrainGenerator& terrain, const Grid& grid, Kind kind, const glm::ivec3& coords, Fn fn) {
    const int x0 = coords.x * S - grid.reach, x1 = coords.x * S + S - 1 + grid.reach;
    const int z0 = coords.z * S - grid.reach, z1 = coords.z * S + S - 1 + grid.reach;
    for (int gx = FloorDiv(x0, grid.size); gx <= FloorDiv(x1, grid.size); gx++)
        for (int gz = FloorDiv(z0, grid.size); gz <= FloorDiv(z1, grid.size); gz++) {
            Candidate c;
            if (!CandidateIn(terrain, grid, kind, gx, gz, c)) continue;
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
    void Leaves(int wx, int wy, int wz, BlockType type = BlockType::LEAVES) const {
        if (Block* b = At(wx, wy, wz); b && (b->IsAir() || b->IsPlant())) *b = Block(type);
    }
    void Solid(int wx, int wy, int wz, BlockType type) const {
        if (Block* b = At(wx, wy, wz)) *b = Block(type);
    }
    // Leaves at ground level may also replace a sloped grass top, but never stone or
    // logs (that would leave whatever is above hanging over transparent leaves).
    void GroundLeaves(int wx, int wy, int wz, BlockType type = BlockType::LEAVES) const {
        Block* b = At(wx, wy, wz);
        if (b && (b->IsPassable() || (IsGrassType(b->type) && !b->IsFullBlock()))) *b = Block(type);
    }
};

// Root must be on intact, dry ground: not a beach or sea floor, below the tree line,
// and no cave entrance under or at the surface.
bool OnSolidGround(int wx, int wz, int surfaceRow) {
    if (TerrainGenerator::IsSandyRow(surfaceRow)) return false;
    if (surfaceRow >= TerrainGenerator::kRockLine) return false;
    return !Caves::IsCave(wx, surfaceRow, wz, surfaceRow) && !Caves::IsCave(wx, surfaceRow - 1, wz, surfaceRow);
}

// A trunk from the ground up (replacing the sloped top it stands on); returns its top.
int Trunk(const Writer& w, int wx, int base, int height, int wz) {
    const int top = base + height - 1;
    for (int y = base; y <= top; y++) w.Solid(wx, y, wz, BlockType::LOG);
    return top;
}

// Broadleaf: two wide layers round the upper trunk, two narrow ones on top.
void Oak(const Candidate& c, const Writer& w, int base, int height, int wideRadius) {
    const int top = Trunk(w, c.wx, base, height, c.wz);
    for (int y = top - 2; y <= top + 1; y++) {
        const int radius = y <= top - 1 ? wideRadius : wideRadius - 1;
        for (int dx = -radius; dx <= radius; dx++)
            for (int dz = -radius; dz <= radius; dz++) {
                const bool corner = std::abs(dx) == radius && std::abs(dz) == radius;
                // Wide layers lose random corners; the very top is a plus shape.
                if (corner && (y == top + 1 || Noise::Random01(c.wx + dx, y, c.wz + dz, c.hash) < 0.6f)) continue;
                if (radius >= 3 && dx * dx + dz * dz > 10) continue; // big crowns are round
                w.Leaves(c.wx + dx, y, c.wz + dz);
            }
    }
}

// Pine: a tall trunk in a narrow cone of needles, wide and narrow layers alternating.
void Pine(const Candidate& c, const Writer& w, int base, int height) {
    const int top = Trunk(w, c.wx, base, height, c.wz);
    for (int y = base + 2; y <= top + 1; y++) {
        const int fromTop = top + 1 - y;
        const int radius = fromTop <= 1 ? (fromTop == 0 ? 0 : 1) : (fromTop % 2 == 0 ? 2 : 1);
        for (int dx = -radius; dx <= radius; dx++)
            for (int dz = -radius; dz <= radius; dz++) {
                if (radius == 2 && std::abs(dx) == 2 && std::abs(dz) == 2) continue;
                w.Leaves(c.wx + dx, y, c.wz + dz, BlockType::PINE_LEAVES);
            }
    }
}

// Savanna tree: a bare trunk under one wide, flat crown.
void Acacia(const Candidate& c, const Writer& w, int base) {
    const int top = Trunk(w, c.wx, base, 5 + static_cast<int>(c.hash % 2), c.wz);
    for (int dx = -3; dx <= 3; dx++)
        for (int dz = -3; dz <= 3; dz++)
            if (dx * dx + dz * dz <= 8 + static_cast<int>((c.hash >> (dx + 3)) & 1)) w.Leaves(c.wx + dx, top + 1, c.wz + dz);
    for (int dx = -1; dx <= 1; dx++)
        for (int dz = -1; dz <= 1; dz++) w.Leaves(c.wx + dx, top + 2, c.wz + dz);
}

void Cactus(const Candidate& c, const Writer& w, int base) {
    for (int y = base; y < base + 1 + static_cast<int>(c.hash % 3); y++) w.Solid(c.wx, y, c.wz, BlockType::CACTUS);
}

void PlaceTree(const TerrainGenerator& terrain, const Candidate& c, const Writer& w) {
    const int base = terrain.ColumnAt(c.wx, c.wz).surfaceRow;
    if (!OnSolidGround(c.wx, c.wz, base)) return;
    switch (c.biome) {
    case Biome::Taiga:
    case Biome::SnowyTaiga: Pine(c, w, base, 6 + static_cast<int>(c.hash % 4)); break;
    case Biome::Tundra: Pine(c, w, base, 4 + static_cast<int>(c.hash % 2)); break; // stunted
    case Biome::Jungle: Oak(c, w, base, 8 + static_cast<int>(c.hash % 4), 3); break; // tall, with big crowns
    case Biome::Savanna: Acacia(c, w, base); break;
    case Biome::Desert: Cactus(c, w, base); break;
    default: Oak(c, w, base, 4 + static_cast<int>(c.hash % 3), 2); break;
    }
}

void PlaceBush(const TerrainGenerator& terrain, const Candidate& c, const Writer& w) {
    const auto column = terrain.ColumnAt(c.wx, c.wz);
    if (!OnSolidGround(c.wx, c.wz, column.surfaceRow)) return;
    const BlockType leaves = c.biome == Biome::Taiga || c.biome == Biome::SnowyTaiga ? BlockType::PINE_LEAVES : BlockType::LEAVES;
    // A small leafy mound sitting on the ground (replacing the sloped grass top).
    const int base = column.surfaceRow;
    w.GroundLeaves(c.wx, base, c.wz, leaves);
    w.Leaves(c.wx, base + 1, c.wz, leaves);
    for (const auto& d : { glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1) }) {
        if (Noise::Random01(c.wx + d.x, base, c.wz + d.y, c.hash) < 0.65f) {
            const int ground = terrain.ColumnAt(c.wx + d.x, c.wz + d.y).surfaceRow;
            if (ground == base && OnSolidGround(c.wx + d.x, c.wz + d.y, ground)) w.GroundLeaves(c.wx + d.x, ground, c.wz + d.y, leaves);
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
    // columns can differ a little from this chunk's, hence the margins (jungle trees
    // reach about 13 blocks up).
    const auto [lo, hi] = std::minmax_element(rows.begin(), rows.end());
    const int bottom = coords.y * S, top = bottom + S - 1;
    if (bottom > *hi + 16 || top < *lo - 4) return;

    const Writer w{ coords * S, blocks };
    ForEachCandidate(terrain, kBoulders, Kind::Boulder, coords, [&](const Candidate& c) { PlaceBoulder(terrain, c, w); });
    ForEachCandidate(terrain, kBushes, Kind::Bush, coords, [&](const Candidate& c) { PlaceBush(terrain, c, w); });
    ForEachCandidate(terrain, kTrees, Kind::Tree, coords, [&](const Candidate& c) { PlaceTree(terrain, c, w); });
}

void OccupiedColumns(const TerrainGenerator& terrain, const glm::ivec3& coords, std::array<bool, S * S>& out) {
    out.fill(false);
    auto mark = [&](const Grid& grid, Kind kind, int reach) {
        // Candidates whose reach (for plants' purposes) touches the chunk.
        const int x0 = coords.x * S - reach, x1 = coords.x * S + S - 1 + reach;
        const int z0 = coords.z * S - reach, z1 = coords.z * S + S - 1 + reach;
        for (int gx = FloorDiv(x0, grid.size); gx <= FloorDiv(x1, grid.size); gx++)
            for (int gz = FloorDiv(z0, grid.size); gz <= FloorDiv(z1, grid.size); gz++) {
                Candidate c;
                if (!CandidateIn(terrain, grid, kind, gx, gz, c)) continue;
                for (int x = c.wx - reach; x <= c.wx + reach; x++)
                    for (int z = c.wz - reach; z <= c.wz + reach; z++) {
                        const int lx = x - coords.x * S, lz = z - coords.z * S;
                        if (lx >= 0 && lx < S && lz >= 0 && lz < S) out[lx * S + lz] = true;
                    }
            }
    };
    mark(kTrees, Kind::Tree, 0);
    mark(kBushes, Kind::Bush, 1);
    mark(kBoulders, Kind::Boulder, kBoulders.reach);
}

bool MayOccupyColumn(const TerrainGenerator& terrain, int wx, int wz) {
    auto near = [&](const Grid& grid, Kind kind, int reach) {
        const int gx = FloorDiv(wx, grid.size), gz = FloorDiv(wz, grid.size);
        for (int dx = -1; dx <= 1; dx++)
            for (int dz = -1; dz <= 1; dz++) {
                Candidate c;
                if (CandidateIn(terrain, grid, kind, gx + dx, gz + dz, c) && ColumnNear(c, wx, wz, reach)) return true;
            }
        return false;
    };
    return near(kTrees, Kind::Tree, 0) || near(kBushes, Kind::Bush, 1) || near(kBoulders, Kind::Boulder, kBoulders.reach);
}

} // namespace Features
