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

float TerrainGenerator::GetHeight(int x, int z) const {
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
            for (int y = 0; y < S; y++) {
                const int blockY = coords.y * S + y;
                if (blockY == m) {
                    // Sloped grass top. If the surface is (almost) flat at the bottom of
                    // this cell the cell stays empty and the block below is grass instead.
                    if (sloped) out[Index(x, y, z)] = Block(BlockType::GRASS, edges);
                }
                else if (blockY == m - 1 && !sloped) {
                    out[Index(x, y, z)] = Block(BlockType::GRASS);
                }
                else if (blockY < m && blockY > m - 5) {
                    out[Index(x, y, z)] = Block(BlockType::DIRT);
                }
                else if (blockY < m) {
                    out[Index(x, y, z)] = Block(BlockType::STONE);
                }
            }
        }
    }

    Caves::Carve(coords, rows, out);
    Features::Place(*this, coords, rows, out);
    PlacePlants(coords, heights, out);
}

void TerrainGenerator::PlacePlants(const glm::ivec3& coords, const ColumnHeights& heights, Blocks& out) const {
    const int chunkBottom = coords.y * S;
    for (int x = 0; x < S; x++) {
        for (int z = 0; z < S; z++) {
            const int wx = coords.x * S + x, wz = coords.z * S + z;
            const Column column = ColumnFrom(heights, x, z);

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
