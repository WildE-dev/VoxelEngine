#include "world/TerrainGenerator.h"

#include <algorithm>
#include <cmath>

#include "world/Caves.h"
#include "world/Features.h"
#include "world/Noise.h"

namespace {

constexpr int S = TerrainGenerator::S;
constexpr float kSmoothness = 8.0f; // surface heights snap to eighths of a block

int Index(int x, int y, int z) { return x + S * (y + S * z); }
float Quantise(float h) { return std::round(h * kSmoothness) / kSmoothness; }
// Height above the block's base in eighths, clamped to the valid edge range.
uint8_t EdgeHeight(float h) { return static_cast<uint8_t>(std::clamp(static_cast<int>(h * 8), 0, 8)); }

TerrainGenerator::Column MakeColumn(float h00, float h10, float h01, float h11) {
    TerrainGenerator::Column c;
    // EdgeData order: 0 = (x, z+1), 1 = (x+1, z), 2 = (x, z), 3 = (x+1, z+1).
    c.corners[0] = Quantise(h01);
    c.corners[1] = Quantise(h10);
    c.corners[2] = Quantise(h00);
    c.corners[3] = Quantise(h11);
    c.surfaceRow = static_cast<int>(std::floor(std::min(std::min(c.corners[0], c.corners[1]), std::min(c.corners[2], c.corners[3]))));
    return c;
}

constexpr uint32_t kPlantSeed = 0x91A7C3E5u;

} // namespace

EdgeData TerrainGenerator::Column::EdgesAt(int row) const {
    EdgeData edges;
    for (int i = 0; i < 4; i++) edges.SetTopY(i, EdgeHeight(corners[i] - row));
    return edges;
}

float TerrainGenerator::Column::GroundHeightAtCentre() const {
    // With a sloped top, the ground is that block's surface; otherwise it's the top
    // of the full grass block underneath.
    if (HasSlopedTop()) return surfaceRow + Block(BlockType::GRASS, EdgesAt(surfaceRow)).SurfaceHeight(0.5f, 0.5f);
    return static_cast<float>(surfaceRow);
}

uint8_t TerrainGenerator::Column::SkyLightAbove(int row) const {
    // The light in cell row + 1, coming straight down. Cells in this column: solid
    // ground below the surface row, the (transparent) sloped top if any, then water up
    // to sea level, then open sky. Below sea level the sloped top is waterlogged.
    // Light loses 2 entering each cell holding water.
    const int cell = row + 1;
    if (cell < surfaceRow) return 0;
    const int waterAtOrAbove = kSeaLevel - std::max(cell, FirstOpenRow());
    const bool waterloggedSlope = cell < FirstOpenRow() && surfaceRow < kSeaLevel;
    if (waterAtOrAbove <= 0 && !waterloggedSlope) return kMaxLight;
    int level = kMaxLight - 2 * waterAtOrAbove;
    // The sloped sea floor block under the water is waterlogged: one more water cell.
    if (cell < FirstOpenRow()) level -= 2;
    return static_cast<uint8_t>(std::max(0, level));
}

float TerrainGenerator::GetHeight(int x, int z) const {
    if (type == Type::Empty) return 0.0f;
    float value = 0.0f;
    float freq = 1.0f;
    float amp = 1.0f;
    float ampTotal = 0.0f;

    for (int i = 0; i < OCTAVES; i++) {
        value += Noise::Perlin2(x * freq / SIZE, z * freq / SIZE) * amp;
        ampTotal += amp;
        freq *= 2.0f;
        amp *= 0.5f;
    }

    return value / ampTotal * AMPLITUDE;
}

void TerrainGenerator::ComputeColumnHeights(int chunkX, int chunkZ, ColumnHeights& out) const {
    for (int x = 0; x <= S; x++)
        for (int z = 0; z <= S; z++)
            out[x * (S + 1) + z] = GetHeight(chunkX * S + x, chunkZ * S + z);
}

TerrainGenerator::Column TerrainGenerator::ColumnFrom(const ColumnHeights& h, int lx, int lz) {
    auto at = [&](int x, int z) { return h[x * (S + 1) + z]; };
    return MakeColumn(at(lx, lz), at(lx + 1, lz), at(lx, lz + 1), at(lx + 1, lz + 1));
}

TerrainGenerator::Column TerrainGenerator::ColumnAt(int wx, int wz) const {
    return MakeColumn(GetHeight(wx, wz), GetHeight(wx + 1, wz), GetHeight(wx, wz + 1), GetHeight(wx + 1, wz + 1));
}

void TerrainGenerator::ComputeSurfaceRows(const ColumnHeights& heights, SurfaceRows& out) {
    for (int x = 0; x < S; x++)
        for (int z = 0; z < S; z++)
            out[x * S + z] = ColumnFrom(heights, x, z).surfaceRow;
}

void TerrainGenerator::GenerateChunk(const glm::ivec3& coords, const ColumnHeights& heights, Blocks& out) const {
    out.fill(Block());
    SurfaceRows rows;
    ComputeSurfaceRows(heights, rows);

    // --- Base terrain: sloped grass surface over dirt over stone ---
    for (int x = 0; x < S; x++) {
        for (int z = 0; z < S; z++) {
            const Column column = ColumnFrom(heights, x, z);
            const int m = column.surfaceRow;
            const EdgeData edges = column.EdgesAt(m);
            const bool sloped = edges.IsValid();
            // Near and below sea level the ground is sand instead of grass and dirt.
            const BlockType top = column.IsSandy() ? BlockType::SAND : BlockType::GRASS;
            const BlockType under = column.IsSandy() ? BlockType::SAND : BlockType::DIRT;
            for (int y = 0; y < S; y++) {
                const int blockY = coords.y * S + y;
                if (blockY == m) {
                    // Sloped top. If the surface is (almost) flat at the bottom of this
                    // cell the cell stays empty and the block below is the top instead.
                    if (sloped) out[Index(x, y, z)] = Block(top, edges);
                }
                else if (blockY == m - 1 && !sloped) {
                    out[Index(x, y, z)] = Block(top);
                }
                else if (blockY < m && blockY > m - 5) {
                    out[Index(x, y, z)] = Block(under);
                }
                else if (blockY < m) {
                    out[Index(x, y, z)] = Block(BlockType::STONE);
                }
            }
        }
    }

    if (type == Type::Empty) return; // just the ground

    Caves::Carve(coords, rows, out);

    // Seas and lakes: open cells above the ground and below sea level, plus the open
    // space above a sloped sea floor (waterlogged). Caves are underground (below the
    // surface row), so they stay dry.
    for (int x = 0; x < S; x++) {
        for (int z = 0; z < S; z++) {
            const Column column = ColumnFrom(heights, x, z);
            const int firstOpen = column.FirstOpenRow();
            for (int y = 0; y < S; y++) {
                const int blockY = coords.y * S + y;
                Block& cell = out[Index(x, y, z)];
                if (blockY >= firstOpen && blockY < kSeaLevel) cell = Block::Water(Block::kMaxWaterVolume);
                else if (blockY == column.surfaceRow && blockY < kSeaLevel && cell.IsPartial()) cell.water = cell.shown = cell.WaterCapacity();
            }
        }
    }

    Features::Place(*this, coords, rows, out);
    PlacePlants(coords, heights, out);
}

void TerrainGenerator::PlacePlants(const glm::ivec3& coords, const ColumnHeights& heights, Blocks& out) const {
    const int chunkBottom = coords.y * S;
    for (int x = 0; x < S; x++) {
        for (int z = 0; z < S; z++) {
            const int wx = coords.x * S + x, wz = coords.z * S + z;
            const Column column = ColumnFrom(heights, x, z);
            if (column.IsSandy()) continue; // grass and flowers only grow on grass

            // The plant stands in the first air cell above the ground. On a sloped top
            // that's the cell above the slope, lowered so it sits on the surface.
            const bool sloped = column.HasSlopedTop();
            const int plantRow = sloped ? column.surfaceRow + 1 : column.surfaceRow;
            const int groundRow = plantRow - 1;
            const int py = plantRow - chunkBottom;
            if (py < 0 || py >= S) continue;

            // Meadows are grassier; flowers are rarer.
            const float meadow = Noise::Perlin2(wx / 40.0f + 300.0f, wz / 40.0f - 700.0f);
            const float grassChance = std::clamp(0.10f + meadow * 0.5f, 0.02f, 0.35f);
            const float r = Noise::Random01(wx, 0, wz, kPlantSeed);
            BlockType type;
            if (r < grassChance) type = BlockType::TALL_GRASS;
            else if (r < grassChance + 0.012f) type = BlockType::FLOWER_RED;
            else if (r < grassChance + 0.024f) type = BlockType::FLOWER_YELLOW;
            else continue;

            if (Features::MayOccupyColumn(wx, wz)) continue;
            if (!out[Index(x, py, z)].IsAir()) continue;

            // The ground must still be grass: not carved into a cave entrance.
            const int gy = groundRow - chunkBottom;
            const bool groundIntact = gy >= 0 ? out[Index(x, gy, z)].type == BlockType::GRASS
                                              : !Caves::IsCave(wx, groundRow, wz, column.surfaceRow);
            if (!groundIntact) continue;

            const float drop = plantRow - column.GroundHeightAtCentre(); // 0..1 blocks
            const int dropEighths = std::clamp(static_cast<int>(std::lround(drop * 8.0f)), 0, 8);
            out[Index(x, py, z)] = Block::Plant(type, static_cast<uint8_t>(dropEighths));
        }
    }
}
