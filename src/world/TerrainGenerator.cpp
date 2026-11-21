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

uint8_t TerrainGenerator::Column::SkyLightAbove(int row, int waterLevel) const {
    // The light in cell row + 1, coming straight down. Cells in this column: solid
    // ground below the surface row, the (transparent) sloped top if any, then water up
    // to sea level, then open sky. Below sea level the sloped top is waterlogged.
    // Light loses 2 entering each cell holding water.
    const int cell = row + 1;
    if (cell < surfaceRow) return 0;
    const int waterAtOrAbove = waterLevel - std::max(cell, FirstOpenRow());
    const bool waterloggedSlope = cell < FirstOpenRow() && surfaceRow < waterLevel;
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
    c.temperature -= std::max(0, surfaceRow - 12) / 160.0f; // colder higher up (snow on the high peaks)
    return c;
}

Biome TerrainGenerator::Classify(const Climate& c) {
    const float t = c.temperature, h = c.humidity;
    if (t < -0.42f) return h < 0.0f ? Biome::Tundra : Biome::SnowyTaiga;
    if (t < -0.2f) return Biome::Taiga;
    if (t < 0.22f) return h < -0.22f ? Biome::Plains : h < 0.32f ? Biome::Forest : Biome::Swamp;
    return h < -0.18f ? Biome::Desert : h < 0.2f ? Biome::Savanna : Biome::Jungle;
}

float TerrainGenerator::GetBaseHeight(int x, int z) const {
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

const TerrainGenerator::Lake& TerrainGenerator::LakeIn(int cx, int cz) const {
    const int64_t key = (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cz);
    {
        std::lock_guard<std::mutex> lock(lakeCache->mutex);
        auto it = lakeCache->lakes.find(key);
        if (it != lakeCache->lakes.end()) return it->second;
    }
    Lake lake;
    constexpr uint32_t kLakeSeed = 0x1A4E5u;
    if (type == Type::Normal && Noise::Random01(cx, 77, cz, kLakeSeed) < 0.8f) {
        // Somewhere near the cell's middle (kept far enough in that neighbours' lakes
        // and their rims never meet).
        const float room = kLakeCell * 0.5f - kLakeMaxRadius * 1.5f - 1.0f;
        lake.radius = kLakeMinRadius + (kLakeMaxRadius - kLakeMinRadius) * Noise::Random01(cx, 78, cz, kLakeSeed);
        lake.centre = glm::vec2(cx * kLakeCell, cz * kLakeCell) + kLakeCell * 0.5f +
                      glm::vec2(Noise::Random01(cx, 79, cz, kLakeSeed) * 2.0f - 1.0f, Noise::Random01(cx, 80, cz, kLakeSeed) * 2.0f - 1.0f) * room;
        lake.depth = 3.0f + lake.radius * 0.15f;
        // The level: near the lowest ground around it, so the rim needs little raising.
        const float centreHeight = GetBaseHeight(static_cast<int>(lake.centre.x), static_cast<int>(lake.centre.y));
        float ringMin = 1e9f, lowestAngle = 0.0f;
        for (int i = 0; i < 20; i++) {
            const float a = i * 6.2831853f / 20.0f;
            const glm::vec2 p = lake.centre + glm::vec2(std::cos(a), std::sin(a)) * lake.radius * 1.2f;
            const float h = GetBaseHeight(static_cast<int>(p.x), static_cast<int>(p.y));
            if (h < ringMin) ringMin = h, lowestAngle = a;
        }
        lake.level = static_cast<int>(std::floor(std::min(ringMin + 1.5f, centreHeight + 2.0f)));
        // Only in the hills and mountains (lowland hollows have the sea and swamps), and
        // not hanging off a cliff.
        lake.exists = lake.level > kSeaLevel + 5 && centreHeight - lake.level < 12.0f && lake.level < kSnowLine + 30;
        // Its overflow: just past the rim on the lowest side, if the land there is
        // below the water (so the stream runs away downhill) but above the sea.
        if (lake.exists) {
            const glm::vec2 p = lake.centre + glm::vec2(std::cos(lowestAngle), std::sin(lowestAngle)) * (lake.radius * 1.5f + 2.0f);
            const glm::ivec2 column(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)));
            const float h = GetBaseHeight(column.x, column.y);
            lake.hasSpring = h < lake.level - 1.0f && h > kSeaLevel + 1.0f;
            lake.spring = column;
        }
    }
    std::lock_guard<std::mutex> lock(lakeCache->mutex);
    if (lakeCache->lakes.size() > 65536) lakeCache->lakes.clear(); // (far away: worked out again if needed)
    return lakeCache->lakes.emplace(key, lake).first->second;
}

int TerrainGenerator::LakeLevelAt(int x, int z) const {
    if (type == Type::Empty) return INT_MIN;
    const glm::vec2 p(x + 0.5f, z + 0.5f);
    const int cx = static_cast<int>(std::floor(p.x / kLakeCell)), cz = static_cast<int>(std::floor(p.y / kLakeCell));
    const Lake& lake = LakeIn(cx, cz);
    // (A column straddling the shore counts, so its sloped top is waterlogged.)
    if (lake.exists && glm::distance(p, lake.centre) < lake.radius + 1.5f) return lake.level;
    return INT_MIN;
}

std::vector<glm::ivec2> TerrainGenerator::SpringsIn(int chunkX, int chunkZ) const {
    std::vector<glm::ivec2> out;
    if (type != Type::Normal) return out;
    // A lake's spring is within 1.5 radii (and a little) of its centre, which is in its
    // own cell: the cells around this chunk's.
    const int x0 = chunkX * S, z0 = chunkZ * S;
    const int cx0 = static_cast<int>(std::floor((x0 - kLakeCell) / static_cast<float>(kLakeCell)));
    const int cz0 = static_cast<int>(std::floor((z0 - kLakeCell) / static_cast<float>(kLakeCell)));
    for (int cx = cx0; cx <= cx0 + 2; cx++)
        for (int cz = cz0; cz <= cz0 + 2; cz++) {
            const Lake& lake = LakeIn(cx, cz);
            if (!lake.exists || !lake.hasSpring) continue;
            if (lake.spring.x >= x0 && lake.spring.x < x0 + S && lake.spring.y >= z0 && lake.spring.y < z0 + S) out.push_back(lake.spring);
        }
    return out;
}

float TerrainGenerator::GetHeight(int x, int z) const {
    const float base = GetBaseHeight(x, z);
    if (type == Type::Empty) return base;
    const glm::vec2 p(static_cast<float>(x), static_cast<float>(z));
    const int cx = static_cast<int>(std::floor(p.x / kLakeCell)), cz = static_cast<int>(std::floor(p.y / kLakeCell));
    const Lake& lake = LakeIn(cx, cz);
    if (!lake.exists) return base;
    const float d = glm::distance(p, lake.centre), r = lake.radius;
    const float level = static_cast<float>(lake.level);
    if (d < r) {
        // The bowl under the water; on the uphill side the shore eases into the slope.
        const float bowl = 1.0f - (d / r) * (d / r);
        float h = level - 0.5f - lake.depth * bowl;
        if (base > h) h += glm::smoothstep(0.7f * r, r, d) * (base - h);
        return h;
    }
    if (d < 1.5f * r) {
        // The rim: just above the water, then down to the land around.
        const float rim = d < 1.1f * r ? level + 0.5f * (d - r) / (0.1f * r) : level + 0.5f + (base - level - 0.5f) * glm::smoothstep(1.1f * r, 1.5f * r, d);
        return std::max(base, rim);
    }
    return base;
}

float TerrainGenerator::GetMountainHeight(int x, int z) const {
    // Where the ranges run: a very large-scale field, high in over a third of the
    // world, fading in over a few hundred blocks so ranges rise out of the hills.
    const float range = Noise::Perlin2(x / 900.0f + 31.7f, z / 900.0f - 77.3f);
    const float mask = glm::smoothstep(-0.14f, 0.44f, range);
    if (mask <= 0.0f) return 0.0f;
    // Ridges inside them: crests where the noise crosses zero, valleys between, with
    // smaller ridges on the flanks. The crests are rounded (smoothstep, not |n|) and the
    // scales wide enough that slopes stay mostly under 45 degrees: each column's
    // surface is one sloped block, so anything much steeper shows as steps.
    auto ridge = [](float n) { return 1.0f - glm::smoothstep(0.0f, 0.6f, std::abs(n)); };
    const float big = ridge(Noise::Perlin2(x / 620.0f + 11.3f, z / 620.0f + 5.9f));
    const float small = ridge(Noise::Perlin2(x / 250.0f - 3.1f, z / 250.0f + 17.7f));
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
                    // (Empty worlds are plain: no ore.)
                    out[Index(x, y, z)] = Block(type == Type::Empty ? BlockType::STONE : OreAt(coords.x * S + x, blockY, coords.z * S + z, m - blockY));
                }
            }
        }
    }

    if (type == Type::Empty) return; // just the ground

    Caves::Carve(coords, rows, out);

    // Lakes: their water level per column, and their beds kept whole (a cave opening
    // under one would drain it) and sandy.
    std::array<int, S * S> waterLevels;
    for (int x = 0; x < S; x++)
        for (int z = 0; z < S; z++) {
            const int wx = coords.x * S + x, wz = coords.z * S + z;
            const int lake = LakeLevelAt(wx, wz);
            waterLevels[x * S + z] = std::max(static_cast<int>(kSeaLevel), lake);
            const int m = rows[x * S + z];
            if (lake <= m) continue; // dry here (water fills from the surface row up)
            const Column column = ColumnFrom(heights, x, z);
            for (int y = 0; y < S; y++) {
                const int blockY = coords.y * S + y;
                Block& cell = out[Index(x, y, z)];
                if (blockY < m - 4 || blockY > m) continue;
                const bool top = blockY == m || (blockY == m - 1 && !column.HasSlopedTop());
                if (top && blockY == m) {
                    if (column.HasSlopedTop()) cell = Block(BlockType::SAND, column.EdgesAt(m));
                }
                else if (blockY < m) cell = Block(top ? BlockType::SAND : BlockType::DIRT);
            }
        }

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
                const int waterLevel = waterLevels[x * S + z];
                if (blockY >= firstOpen && blockY < waterLevel) {
                    // Frozen biomes: the sea's top layer is ice (mountain lakes stay open).
                    const bool ice = waterLevel == kSeaLevel && blockY == kSeaLevel - 1 && IsFrozen(biomes[x * S + z]);
                    cell = ice ? Block(BlockType::ICE) : Block::Water(Block::kMaxWaterVolume);
                }
                else if (blockY == column.surfaceRow && blockY < waterLevel && cell.IsPartial()) cell.water = cell.shown = cell.WaterCapacity();
            }
        }
    }

    Features::Place(*this, coords, rows, out);
    PlacePlants(coords, heights, rows, out);
    // Springs where lakes overflow: the column's top solid block (under a sloped top, the
    // slope itself), with nothing growing on it.
    for (const glm::ivec2& s : SpringsIn(coords.x, coords.z)) {
        const int x = s.x - coords.x * S, z = s.y - coords.z * S;
        const Column column = ColumnFrom(heights, x, z);
        const int row = column.HasSlopedTop() ? column.surfaceRow : column.surfaceRow - 1;
        const int y = row - coords.y * S;
        if (y >= 0 && y < S) out[Index(x, y, z)] = Block(BlockType::SPRING);
        // (Clear of plants and boulders over it, so the water can come out; a tree's
        // trunk is left standing.)
        if (y + 1 >= 0 && y + 1 < S) {
            Block& over = out[Index(x, y + 1, z)];
            if (!over.IsAir() && !over.IsLiquid() && over.type != BlockType::LOG) over = Block();
        }
    }
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
