#include "world/TerrainGenerator.h"

#include <algorithm>
#include <cmath>

#include "world/Caves.h"
#include "world/Dungeons.h"
#include "world/Features.h"
#include "world/Noise.h"

namespace {

// Stone, or ore in veins through it: blobs of 3D noise, coal from 8 blocks down,
// iron (rarer, in smaller veins) from 24 down. A function of position, so chunks agree.
BlockType OreAt(int x, int y, int z, int depth) {
    if (depth >= 8) {
        const float coal = Noise::Perlin3(x * 0.18f, y * 0.18f, z * 0.18f, 0xC0A1u);
        if (coal > 0.48f) return BlockType::COAL_ORE;
    }
    if (depth >= 24) {
        const float iron = Noise::Perlin3(x * 0.22f, y * 0.22f, z * 0.22f, 0x1205u);
        if (iron > 0.56f) return BlockType::IRON_ORE;
    }
    return BlockType::STONE;
}

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

const char* BiomeName(Biome biome) {
    switch (biome) {
    case Biome::Plains: return "Plains";
    case Biome::Forest: return "Forest";
    case Biome::Swamp: return "Swamp";
    case Biome::Desert: return "Desert";
    case Biome::Savanna: return "Savanna";
    case Biome::Jungle: return "Jungle";
    case Biome::Taiga: return "Taiga";
    case Biome::SnowyTaiga: return "Snowy taiga";
    case Biome::Tundra: return "Tundra";
    default: return "?";
    }
}

BlockType TerrainGenerator::Column::TopType(Biome biome) const {
    if (IsSandy()) return biome == Biome::Swamp ? BlockType::DIRT : BlockType::SAND; // muddy swamp beds, else beaches
    if (surfaceRow >= kSnowLine) return BlockType::SNOW;
    const float steepness = std::max({ corners[0], corners[1], corners[2], corners[3] }) - std::min({ corners[0], corners[1], corners[2], corners[3] });
    if (surfaceRow >= kRockLine || steepness >= 1.5f) return BlockType::STONE; // bare rock high up and on cliffs
    switch (biome) {
    case Biome::Desert: return BlockType::SAND;
    case Biome::Savanna: return BlockType::DRY_GRASS;
    case Biome::Jungle:
    case Biome::Swamp: return BlockType::LUSH_GRASS;
    case Biome::SnowyTaiga:
    case Biome::Tundra: return BlockType::SNOWY_GRASS;
    default: return BlockType::GRASS;
    }
}

BlockType TerrainGenerator::Column::UnderType(Biome biome) const {
    const BlockType top = TopType(biome);
    if (top == BlockType::SAND) return BlockType::SAND;
    if (IsGrassType(top) || top == BlockType::DIRT) return BlockType::DIRT;
    return BlockType::STONE;
}

TerrainGenerator::Climate TerrainGenerator::BaseClimate(int x, int z) const {
    // Very large-scale fields, so a biome spans hundreds of blocks; a little smaller
    // detail on top roughens their borders.
    const float t = Noise::Perlin2(x / 1100.0f + 404.3f, z / 1100.0f - 251.9f) * 1.5f + Noise::Perlin2(x / 140.0f, z / 140.0f + 9.1f) * 0.08f;
    const float h = Noise::Perlin2(x / 800.0f - 890.7f, z / 800.0f + 333.1f) * 1.5f + Noise::Perlin2(x / 120.0f + 51.0f, z / 120.0f) * 0.08f;
    return { t, h };
}

TerrainGenerator::Climate TerrainGenerator::ClimateAt(int wx, int wz, int surfaceRow) const {
    if (type == Type::Empty) return { 0.0f, -0.6f }; // plains
    Climate c = BaseClimate(wx, wz);
    c.temperature -= std::max(0, surfaceRow - 12) / 70.0f; // colder higher up
    return c;
}

Biome TerrainGenerator::Classify(const Climate& c) {
    const float t = c.temperature, h = c.humidity;
    if (t < -0.42f) return h < 0.0f ? Biome::Tundra : Biome::SnowyTaiga;
    if (t < -0.2f) return Biome::Taiga;
    if (t < 0.22f) return h < -0.22f ? Biome::Plains : h < 0.32f ? Biome::Forest : Biome::Swamp;
    return h < -0.18f ? Biome::Desert : h < 0.2f ? Biome::Savanna : Biome::Jungle;
}

float TerrainGenerator::GetHeight(int x, int z) const {
    if (type == Type::Empty) return 0.0f;
    const float land = GetHillHeight(x, z) + GetMountainHeight(x, z);
    // Swamps (temperate and very wet) sink toward sea level, rising and dipping just
    // around it, so the sea fills their hollows with shallow standing water. The weight
    // fades in over a band of climate, so the land slopes down into them.
    const Climate c = BaseClimate(x, z);
    const float swamp = glm::smoothstep(0.22f, 0.32f, c.humidity) * glm::smoothstep(-0.2f, -0.12f, c.temperature) *
                        (1.0f - glm::smoothstep(0.14f, 0.22f, c.temperature));
    if (swamp <= 0.0f) return land;
    const float marsh = kSeaLevel - 0.1f + 1.4f * Noise::Perlin2(x / 21.0f + 7.7f, z / 21.0f - 3.3f);
    return land + (marsh - land) * swamp;
}

float TerrainGenerator::GetMountainHeight(int x, int z) const {
    // Where the ranges run: a very large-scale field, high in about a fifth of the
    // world, fading in over a few hundred blocks so ranges rise out of the hills.
    const float range = Noise::Perlin2(x / 900.0f + 31.7f, z / 900.0f - 77.3f);
    const float mask = glm::smoothstep(0.12f, 0.42f, range);
    if (mask <= 0.0f) return 0.0f;
    // Ridges inside them: crests where the noise crosses zero, valleys between, with
    // smaller ridges on the flanks. The crests are rounded (smoothstep, not |n|) and the
    // scales wide enough that slopes stay mostly under 45 degrees: each column's
    // surface is one sloped block, so anything much steeper shows as steps.
    auto ridge = [](float n) { return 1.0f - glm::smoothstep(0.0f, 0.6f, std::abs(n)); };
    const float big = ridge(Noise::Perlin2(x / 430.0f + 11.3f, z / 430.0f + 5.9f));
    const float small = ridge(Noise::Perlin2(x / 170.0f - 3.1f, z / 170.0f + 17.7f));
    return mask * kMountainHeight * big * (0.8f + 0.25f * small);
}

float TerrainGenerator::GetHillHeight(int x, int z) const {
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

    // Each column's biome, where this chunk holds its surface or its sea's top (deep
    // chunks are all stone and don't need it).
    std::array<Biome, S * S> biomes;
    const int bottom = coords.y * S, topRow = bottom + S - 1;
    for (int x = 0; x < S; x++)
        for (int z = 0; z < S; z++) {
            const int m = rows[x * S + z];
            const bool needed = topRow >= std::min(m - 6, kSeaLevel - 1) && bottom <= std::max(m + 1, kSeaLevel);
            biomes[x * S + z] = needed ? BiomeAt(coords.x * S + x, coords.z * S + z, m) : Biome::Plains;
        }

    // --- Base terrain: the biome's sloped surface over dirt (or sand) over stone ---
    for (int x = 0; x < S; x++) {
        for (int z = 0; z < S; z++) {
            const Column column = ColumnFrom(heights, x, z);
            const int m = column.surfaceRow;
            const EdgeData edges = column.EdgesAt(m);
            const bool sloped = edges.IsValid();
            // Beaches, rock and snow on mountains, else the biome's ground.
            const BlockType top = column.TopType(biomes[x * S + z]);
            const BlockType under = column.UnderType(biomes[x * S + z]);
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
                    out[Index(x, y, z)] = Block(OreAt(coords.x * S + x, blockY, coords.z * S + z, m - blockY));
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
                if (blockY >= firstOpen && blockY < kSeaLevel) {
                    // Frozen biomes: the sea's top layer is ice.
                    const bool ice = blockY == kSeaLevel - 1 && IsFrozen(biomes[x * S + z]);
                    cell = ice ? Block(BlockType::ICE) : Block::Water(Block::kMaxWaterVolume);
                }
                else if (blockY == column.surfaceRow && blockY < kSeaLevel && cell.IsPartial()) cell.water = cell.shown = cell.WaterCapacity();
            }
        }
    }

    Features::Place(*this, coords, rows, out);
    PlacePlants(coords, heights, rows, out);
    Dungeons::Stamp(*this, coords, out);
}

void TerrainGenerator::PlacePlants(const glm::ivec3& coords, const ColumnHeights& heights, const SurfaceRows& rows, Blocks& out) const {
    const int chunkBottom = coords.y * S;
    // Plants stand just above the surface: nothing to do in chunks away from it.
    const auto [lo, hi] = std::minmax_element(rows.begin(), rows.end());
    if (chunkBottom > *hi + 1 || chunkBottom + S - 1 < *lo) return;
    // Columns a tree, bush or boulder may take, worked out once for the chunk.
    std::array<bool, S * S> occupied;
    Features::OccupiedColumns(*this, coords, occupied);
    for (int x = 0; x < S; x++) {
        for (int z = 0; z < S; z++) {
            const int wx = coords.x * S + x, wz = coords.z * S + z;
            const Column column = ColumnFrom(heights, x, z);
            {
                const int plantRow = column.HasSlopedTop() ? column.surfaceRow + 1 : column.surfaceRow;
                if (plantRow < chunkBottom || plantRow >= chunkBottom + S) continue;
            }
            const Biome biome = BiomeAt(wx, wz, column.surfaceRow);
            const BlockType ground = column.TopType(biome);
            // Grass and flowers grow on grass; in deserts, the odd dead bush on sand.
            const bool desert = biome == Biome::Desert && ground == BlockType::SAND;
            // Snowfields: only the odd dead bush pokes through.
            const bool snowfield = ground == BlockType::SNOWY_GRASS;
            // Pebbles lie about anywhere on solid ground, more on beaches and bare rock.
            const bool rocky = ground == BlockType::STONE || ground == BlockType::SAND;
            const bool pebbles = (IsGrassType(ground) || rocky || ground == BlockType::DIRT) &&
                                 Noise::Random01(wx, 1, wz, kPlantSeed) < (rocky ? 0.035f : 0.008f);
            if (!IsGrassType(ground) && !desert && !pebbles) continue;

            // The plant stands in the first air cell above the ground. On a sloped top
            // that's the cell above the slope, lowered so it sits on the surface.
            const bool sloped = column.HasSlopedTop();
            const int plantRow = sloped ? column.surfaceRow + 1 : column.surfaceRow;
            const int groundRow = plantRow - 1;
            const int py = plantRow - chunkBottom;
            if (py < 0 || py >= S) continue;

            // How much grows, by biome; meadows are grassier within each.
            float grass = 0.15f, flowers = 0.02f;
            switch (biome) {
            case Biome::Plains: grass = 0.28f; flowers = 0.06f; break;
            case Biome::Forest: grass = 0.14f; flowers = 0.02f; break;
            case Biome::Swamp: grass = 0.35f; flowers = 0.005f; break;
            case Biome::Desert: grass = 0.0f; flowers = 0.0f; break;
            case Biome::Savanna: grass = 0.26f; flowers = 0.006f; break;
            case Biome::Jungle: grass = 0.5f; flowers = 0.03f; break;
            case Biome::Taiga: grass = 0.12f; flowers = 0.004f; break;
            case Biome::SnowyTaiga:
            case Biome::Tundra: grass = 0.0f; flowers = 0.0f; break; // nothing green in the snow
            default: break;
            }
            const float meadow = Noise::Perlin2(wx / 40.0f + 300.0f, wz / 40.0f - 700.0f);
            const float grassChance = std::clamp(grass + meadow * grass * 1.5f, grass * 0.2f, grass * 2.0f);
            const float r = Noise::Random01(wx, 0, wz, kPlantSeed);
            BlockType type;
            if (pebbles) type = BlockType::PEBBLES;
            else if (desert || snowfield) {
                if (r >= (desert ? 0.02f : 0.01f)) continue;
                type = BlockType::DEAD_BUSH;
            }
            else if (r < grassChance) type = BlockType::TALL_GRASS;
            else if (r < grassChance + flowers * 0.5f) type = BlockType::FLOWER_RED;
            else if (r < grassChance + flowers) type = BlockType::FLOWER_YELLOW;
            else continue;

            if (occupied[x * S + z]) continue;
            if (!out[Index(x, py, z)].IsAir()) continue;

            // The ground must still be grass: not carved into a cave entrance.
            const int gy = groundRow - chunkBottom;
            const bool groundIntact = gy >= 0 ? out[Index(x, gy, z)].type == ground
                                              : !Caves::IsCave(wx, groundRow, wz, column.surfaceRow);
            if (!groundIntact) continue;

            const float drop = plantRow - column.GroundHeightAtCentre(); // 0..1 blocks
            const int dropEighths = std::clamp(static_cast<int>(std::lround(drop * 8.0f)), 0, 8);
            out[Index(x, py, z)] = Block::Plant(type, static_cast<uint8_t>(dropEighths));
        }
    }
}
