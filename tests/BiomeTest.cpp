// Biomes. Classifies a big stretch of the world and generates chunks deep inside each
// biome, checking that
//  - every biome turns up, none is a sliver, and spawn is temperate,
//  - climate changes smoothly (no jumps between neighbouring columns) and gets colder
//    with height,
//  - each biome has its ground: sand in deserts, dry grass on the savanna, lush grass in
//    jungles and swamps, snowy grass in the frozen north, grass elsewhere,
//  - and its plants: cacti and dead bushes (no trees) in deserts, pines in taigas, far
//    more trees in jungles than on plains,
//  - frozen seas have ice over water, swamps have shallow standing water,
//  - nothing floats: trunks and cacti stand on something, plants on their ground,
//  - pebbles lie about on solid ground,
//  - dwarf camps and mines are built whole (a camp's fire, chest and palisade; a
//    mine's shaft, ladder, tunnels and chests),
//  - lakes overflow from springs, whose rivers run in sandy, watered channels to
//    drains.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>
#include <unordered_map>

#include "world/Coords.h"
#include "world/Dwarves.h"
#include "world/Caves.h"
#include "world/TerrainGenerator.h"

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

struct World {
    TerrainGenerator terrain;
    std::unordered_map<glm::ivec3, std::unique_ptr<TerrainGenerator::Blocks>, Coords::IVec3Hash> chunks;
    double seconds = 0.0;
    size_t generated = 0;
    const Block& Get(const glm::ivec3& p) {
        const glm::ivec3 c = Coords::ChunkOf(p);
        auto& chunk = chunks[c];
        if (!chunk) {
            chunk = std::make_unique<TerrainGenerator::Blocks>();
            TerrainGenerator::ColumnHeights heights;
            const auto t0 = std::chrono::steady_clock::now();
            terrain.ComputeColumnHeights(c.x, c.z, heights);
            terrain.GenerateChunk(c, heights, *chunk);
            seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            generated++;
        }
        const glm::ivec3 l = Coords::LocalOf(p);
        return (*chunk)[Chunk::Index(l.x, l.y, l.z)];
    }
};

} // namespace

int main() {
    World world;
    const TerrainGenerator& t = world.terrain;
    constexpr int kBiomes = static_cast<int>(Biome::Count);

    std::cout << "Climate" << std::endl;
    // Coverage, and for each biome a spot well inside it (every sample point 24 blocks
    // around is the same biome), nearest spawn first.
    long counts[kBiomes] = {}, land = 0;
    glm::ivec2 inside[kBiomes];
    bool found[kBiomes] = {};
    long long bestDist[kBiomes];
    std::fill(std::begin(bestDist), std::end(bestDist), 1LL << 40);
    for (int x = -3000; x <= 3000; x += 32)
        for (int z = -3000; z <= 3000; z += 32) {
            const auto c = t.ColumnAt(x, z);
            if (c.surfaceRow < TerrainGenerator::kSeaLevel) continue;
            land++;
            const Biome b = t.BiomeAt(x, z, c.surfaceRow);
            counts[static_cast<int>(b)]++;
            if (c.surfaceRow > 30) continue; // not up a mountain
            const long long d = static_cast<long long>(x) * x + static_cast<long long>(z) * z;
            if (d >= bestDist[static_cast<int>(b)]) continue;
            bool uniform = true;
            for (int dx = -24; dx <= 24 && uniform; dx += 12)
                for (int dz = -24; dz <= 24 && uniform; dz += 12) uniform = t.BiomeAt(x + dx, z + dz) == b;
            if (uniform) bestDist[static_cast<int>(b)] = d, inside[static_cast<int>(b)] = { x, z }, found[static_cast<int>(b)] = true;
        }
    bool all = true, noSliver = true;
    std::string mix;
    for (int b = 0; b < kBiomes; b++) {
        all &= counts[b] > 0 && found[b];
        noSliver &= counts[b] * 50 >= land; // at least 2% of the land
        mix += (b ? ", " : "") + std::string(BiomeName(static_cast<Biome>(b))) + " " + std::to_string(100 * counts[b] / land) + "%";
    }
    Check(all, "every biome turns up within 3000 blocks");
    Check(noSliver, "and covers at least 2% of the land (" + mix + ")");
    const Biome spawn = t.BiomeAt(8, 8);
    Check(spawn == Biome::Forest || spawn == Biome::Plains, std::string("spawn is temperate (") + BiomeName(spawn) + ")");

    float worstJump = 0.0f;
    for (int x = -500; x <= 500; x += 7)
        for (int z = -500; z <= 500; z += 7) {
            const auto a = t.ClimateAt(x, z, 0), b = t.ClimateAt(x + 1, z, 0), c = t.ClimateAt(x, z + 1, 0);
            worstJump = std::max({ worstJump, std::abs(a.temperature - b.temperature), std::abs(a.temperature - c.temperature),
                                   std::abs(a.humidity - b.humidity), std::abs(a.humidity - c.humidity) });
        }
    Check(worstJump < 0.02f, "climate changes smoothly (largest step between neighbours " + std::to_string(worstJump) + ")");
    Check(t.ClimateAt(100, 100, 150).temperature < t.ClimateAt(100, 100, 0).temperature - 0.8f, "and gets colder higher up");

    std::cout << "Biomes on the ground" << std::endl;
    auto expectedTop = [](Biome b) {
        switch (b) {
        case Biome::Desert: return BlockType::SAND;
        case Biome::Savanna: return BlockType::DRY_GRASS;
        case Biome::Jungle:
        case Biome::Swamp: return BlockType::LUSH_GRASS;
        case Biome::SnowyTaiga:
        case Biome::Tundra: return BlockType::SNOWY_GRASS;
        default: return BlockType::GRASS;
        }
    };
    struct Census {
        int columns = 0, rightGround = 0, logs = 0, leaves = 0, pine = 0, cacti = 0, deadBushes = 0, water = 0, deepWater = 0, pebbles = 0;
    };
    std::map<Biome, Census> census;
    bool nothingFloats = true, plantsGrounded = true;
    for (int b = 0; b < kBiomes; b++) {
        const Biome biome = static_cast<Biome>(b);
        Census& n = census[biome];
        const glm::ivec2 at = inside[b];
        for (int x = at.x - 20; x <= at.x + 20; x++)
            for (int z = at.y - 20; z <= at.y + 20; z++) {
                const auto col = t.ColumnAt(x, z);
                if (t.BiomeAt(x, z, col.surfaceRow) != biome) continue;
                n.columns++;
                const BlockType top = col.TopType(biome);
                const bool special = col.IsSandy() || top == BlockType::STONE || top == BlockType::SNOW; // beach, rock, peak
                if (special || top == expectedTop(biome)) n.rightGround++;
                // Water standing at the surface, and how deep.
                int depth = 0;
                for (int y = TerrainGenerator::kSeaLevel - 1; y >= col.surfaceRow - 1 && world.Get({ x, y, z }).HasWater(); y--) depth++;
                if (depth > 0) n.water++;
                if (depth > 3) n.deepWater++;
                for (int y = col.surfaceRow - 1; y <= col.surfaceRow + 16; y++) {
                    const Block& block = world.Get({ x, y, z });
                    const Block& below = world.Get({ x, y - 1, z });
                    if (block.type == BlockType::LOG || block.type == BlockType::CACTUS) {
                        if (block.type == BlockType::LOG) n.logs++;
                        else n.cacti++;
                        if (below.IsPassable() || IsLeavesType(below.type)) nothingFloats = false;
                    }
                    if (block.type == BlockType::LEAVES) n.leaves++;
                    if (block.type == BlockType::PINE_LEAVES) n.pine++;
                    if (block.type == BlockType::DEAD_BUSH) n.deadBushes++;
                    if (block.type == BlockType::PEBBLES) {
                        n.pebbles++;
                        if (!below.IsFullBlock() && !below.IsPartial()) plantsGrounded = false; // on solid ground
                    }
                    else if (block.IsPlant() && block.type != BlockType::SPIKES) {
                        const bool onGrass = IsGrassType(below.type);
                        const bool onSand = below.type == BlockType::SAND && block.type == BlockType::DEAD_BUSH;
                        if (!onGrass && !onSand) plantsGrounded = false;
                    }
                }
            }
    }
    for (int b = 0; b < kBiomes; b++) {
        const Census& n = census[static_cast<Biome>(b)];
        Check(n.columns > 200 && n.rightGround == n.columns,
              std::string(BiomeName(static_cast<Biome>(b))) + " has its ground (" + std::to_string(n.rightGround) + "/" + std::to_string(n.columns) + " columns)");
    }
    const Census& desert = census[Biome::Desert];
    Check(desert.cacti > 0 && desert.deadBushes > 0 && desert.logs == 0 && desert.leaves == 0, "deserts have cacti and dead bushes, and no trees");
    Check(census[Biome::Taiga].pine > 0 && census[Biome::Taiga].leaves == 0 && census[Biome::SnowyTaiga].pine > 0, "taigas grow pines");
    Check(census[Biome::Savanna].leaves > 0 && census[Biome::Savanna].logs < census[Biome::Forest].logs, "the savanna has trees, far fewer than a forest");
    Check(census[Biome::Jungle].logs > 3 * std::max(1, census[Biome::Plains].logs), "jungles have many times the trees of plains (" + std::to_string(census[Biome::Jungle].logs) +
                                                                                          " trunk blocks against " + std::to_string(census[Biome::Plains].logs) + ")");
    const Census& swamp = census[Biome::Swamp];
    Check(swamp.water * 10 > swamp.columns && swamp.deepWater * 50 < swamp.columns,
          "swamps have shallow standing water (" + std::to_string(100 * swamp.water / std::max(1, swamp.columns)) + "% of columns, almost all under 4 deep)");
    Check(nothingFloats, "every trunk and cactus stands on something");
    Check(plantsGrounded, "every plant stands on grass (a dead bush on sand, pebbles on any solid ground)");
    int pebbles = 0, sampled = 0;
    for (const auto& [b, n] : census) pebbles += n.pebbles, sampled += n.columns;
    Check(pebbles * 400 > sampled && pebbles * 30 < sampled,
          "pebbles lie about, here and there (" + std::to_string(pebbles) + " in " + std::to_string(sampled) + " columns)");

    // Frozen seas: the top layer of sea in a frozen biome is ice, with water under it.
    bool sawIce = false, iceRight = true;
    for (int x = -3000; x <= 3000 && !sawIce; x += 16)
        for (int z = -3000; z <= 3000 && !sawIce; z += 16) {
            const auto col = t.ColumnAt(x, z);
            if (col.surfaceRow > TerrainGenerator::kSeaLevel - 3 || !IsFrozen(t.BiomeAt(x, z, col.surfaceRow))) continue;
            sawIce = true;
            iceRight = world.Get({ x, TerrainGenerator::kSeaLevel - 1, z }).type == BlockType::ICE && world.Get({ x, TerrainGenerator::kSeaLevel - 2, z }).IsLiquid();
        }
    Check(sawIce && iceRight, "frozen seas have ice on top, water under it");

    // Springs where lakes overflow: in the ground just outside a lake, below its level,
    // open above so the water can come out (unless a tree grows there).
    int springs = 0, springsRight = 0;
    for (int cx = -60; cx <= 60 && springs < 24; cx++)
        for (int cz = -60; cz <= 60 && springs < 24; cz++)
            for (const glm::ivec2& s : t.SpringsIn(cx, cz)) {
                springs++;
                const auto col = t.ColumnAt(s.x, s.y);
                const int row = col.HasSlopedTop() ? col.surfaceRow : col.surfaceRow - 1;
                bool lakeAbove = false;
                for (int dx = -60; dx <= 60 && !lakeAbove; dx += 3)
                    for (int dz = -60; dz <= 60 && !lakeAbove; dz += 3) lakeAbove = t.LakeLevelAt(s.x + dx, s.y + dz) > row + 1;
                if (world.Get({ s.x, row, s.y }).type == BlockType::SPRING && (!world.Get({ s.x, row + 1, s.y }).IsFullBlock() || world.Get({ s.x, row + 1, s.y }).type == BlockType::LOG) && lakeAbove)
                    springsRight++;
                else
                    std::cout << "        spring at " << s.x << " " << row << " " << s.y << ": " << BlockId(world.Get({ s.x, row, s.y }).type) << ", above "
                              << BlockId(world.Get({ s.x, row + 1, s.y }).type) << (lakeAbove ? "" : ", no lake above") << std::endl;
            }
    Check(springs >= 24, "lakes overflow from springs (" + std::to_string(springs) + " looked at)");
    Check(springsRight == springs, "each spring is in the ground, open above, below a lake's level nearby (" + std::to_string(springsRight) + " of " + std::to_string(springs) + ")");

    // Rivers from them: drains where they end, and channels with sandy beds and water.
    int drains = 0, drainsRight = 0, riverColumns = 0, riverRight = 0;
    for (int cx = -40; cx <= 40 && drains < 16; cx++)
        for (int cz = -40; cz <= 40 && drains < 16; cz++)
            for (const glm::ivec2& d : t.DrainsIn(cx, cz)) {
                drains++;
                const auto col = t.ColumnAt(d.x, d.y);
                const int row = col.HasSlopedTop() ? col.surfaceRow : col.surfaceRow - 1;
                if (world.Get({ d.x, row, d.y }).type == BlockType::SINKHOLE) drainsRight++;
                // The channel upstream of it: columns in the river nearby.
                for (int dx = -12; dx <= 12; dx += 2)
                    for (int dz = -12; dz <= 12; dz += 2) {
                        const int x = d.x + dx, z = d.y + dz;
                        if (!t.InRiver(x, z) || t.LakeLevelAt(x, z) != INT_MIN) continue;
                        const auto c = t.ColumnAt(x, z);
                        if (c.surfaceRow < TerrainGenerator::kSeaLevel) continue; // (out in the sea)
                        if (c.HasSlopedTop()) continue;                            // (a lip or a bank: dry until the spring's water comes)
                        riverColumns++;
                        const Block& top = world.Get({ x, c.HasSlopedTop() ? c.surfaceRow : c.surfaceRow - 1, z });
                        const Block& over = world.Get({ x, c.FirstOpenRow(), z });
                        if ((top.type == BlockType::SAND || top.type == BlockType::SINKHOLE || top.type == BlockType::SPRING) && (over.HasWater() || top.HasWater())) riverRight++;
                    }
            }
    Check(drains >= 16 && drainsRight == drains, "rivers end in sinkholes (natural drains) in their beds (" + std::to_string(drainsRight) + " of " + std::to_string(drains) + ")");
    // Each sinkhole knows its spring and the spring its sinkhole (the water simulation
    // runs the one only while the other is in its area).
    {
        int ends = 0, endsRight = 0;
        for (int cx = -40; cx <= 40 && ends < 16; cx++)
            for (int cz = -40; cz <= 40 && ends < 16; cz++)
                for (const glm::ivec2& d : t.DrainsIn(cx, cz)) {
                    ends++;
                    const auto spring = t.RiverOtherEnd(d.x, d.y);
                    const auto back = spring ? t.RiverOtherEnd(spring->x, spring->z) : std::nullopt;
                    if (spring && back && back->x == d.x && back->z == d.y && glm::distance(glm::vec2(spring->x, spring->z), glm::vec2(d)) >= 10.0f) endsRight++;
                }
        Check(ends >= 16 && endsRight == ends && !t.RiverOtherEnd(3, 3), "a river's spring and sinkhole each know the other (" + std::to_string(endsRight) + " of " + std::to_string(ends) + ")");
    }
    Check(riverColumns > 50 && riverRight * 10 >= riverColumns * 9,
          "river pools have sand beds with water over them (" + std::to_string(riverRight) + " of " + std::to_string(riverColumns) + " pool columns looked at)");
    {
        // No river runs into a lake (its own, back over the rim, or another's), which it
        // would drain; and none ends in a sinkhole under the sea, which would drain that:
        // a river that reaches the sea just runs into it.
        int inRiver = 0, inLake = 0;
        for (int x = -900; x < 900; x += 2)
            for (int z = -900; z < 900; z += 2) {
                if (!t.InRiver(x, z)) continue;
                inRiver++;
                if (t.LakeLevelAt(x, z) != INT_MIN) inLake++;
            }
        Check(inRiver > 500 && inLake == 0, "no river runs into a lake (" + std::to_string(inLake) + " of " + std::to_string(inRiver) + " river columns looked at)");
        int sinkholes = 0, underSea = 0;
        for (int cx = -40; cx <= 40; cx++)
            for (int cz = -40; cz <= 40; cz++)
                for (const glm::ivec2& d : t.DrainsIn(cx, cz)) {
                    sinkholes++;
                    // (At the sea's edge, much of the ground round it is under the sea. River
                    // channels, its own and others', don't count: they're cut below it.)
                    int wet = 0;
                    for (int k = 0; k < 32; k++) {
                        const float a = k * 6.2831853f / 32.0f;
                        const int x = d.x + static_cast<int>(std::lround(10.0f * std::cos(a))), z = d.y + static_cast<int>(std::lround(10.0f * std::sin(a)));
                        wet += t.GetHeight(x, z) < TerrainGenerator::kSeaLevel && !t.InRiver(x, z, TerrainGenerator::kRiverBanks);
                    }
                    if (wet >= 8) underSea++;
                }
        Check(sinkholes > 16 && underSea == 0, "no sinkhole is out in the sea (" + std::to_string(underSea) + " of " + std::to_string(sinkholes) + ")");
    }
    std::cout << "        " << world.generated << " chunks generated, " << world.seconds * 1000.0 / world.generated << " ms each" << std::endl;

    // Dwarf camps and mines: a camp's fire, chest and palisade; a mine's open shaft with
    // its ladder, its tunnels, and a chest at each tunnel's end.
    int camps = 0, mines = 0, campsRight = 0, minesRight = 0, markets = 0, marketsRight = 0, strongholds = 0, strongholdsRight = 0, forges = 0, forgesRight = 0, deepShafts = 0, deepRight = 0;
    for (int rx = -6; rx <= 6; rx++)
        for (int rz = -6; rz <= 6; rz++) {
            const auto r = Dwarves::SiteIn(t, rx, rz);
            if (!r.exists) continue;
            const glm::ivec3 c = r.site.centre;
            bool right = true;
            for (const auto& chest : r.site.chests) right = right && world.Get(chest).type == BlockType::CHEST;
            if (r.site.kind == Dwarves::SiteKind::Camp) {
                if (camps >= 4) continue;
                camps++;
                right = right && world.Get(c).type == BlockType::CAMPFIRE;
                int logs = 0;
                for (int d = -7; d <= 7; d++) logs += (world.Get(c + glm::ivec3(d, 0, 7)).type == BlockType::LOG) + (world.Get(c + glm::ivec3(7, 0, d)).type == BlockType::LOG);
                right = right && logs >= 3;
                campsRight += right;
            }
            else if (r.site.kind == Dwarves::SiteKind::Stronghold) {
                // A stronghold: its hall open, the throne at the far end, the vault's
                // chests, and the stairway open to the sky at the top.
                if (strongholds >= 2) continue;
                strongholds++;
                const glm::ivec3 g = r.gate, top = c + g * (Dwarves::kHallHalf + 1 + r.stairs) + glm::ivec3(0, r.stairs, 0);
                right = right && world.Get(c + glm::ivec3(0, 3, 0)).IsAir() && world.Get(c - g * Dwarves::kHallHalf + glm::ivec3(0, 2, 0)).type == BlockType::LOG &&
                        r.site.chests.size() == 2 && world.Get(top + glm::ivec3(0, 1, 0)).IsAir() && world.Get(top - g * 5 + glm::ivec3(0, -4, 0)).IsAir();
                right = right && world.Get(c + g * (Dwarves::kHallHalf - 2) + glm::ivec3(0, -1, 0)).type == BlockType::CRUMBLING_BRICK &&
                        world.Get(c + g * (Dwarves::kHallHalf - 2) + glm::ivec3(0, -2, 0)).type == BlockType::SPIKES;
                strongholdsRight += right;
            }
            else if (r.site.kind == Dwarves::SiteKind::Market) {
                // A market: a fire, lamps by the gate, and no loot.
                if (markets >= 2) continue;
                markets++;
                const glm::ivec3 side(r.gate.z, 0, -r.gate.x);
                right = world.Get(c).type == BlockType::CAMPFIRE && r.site.chests.empty() && r.site.traders == 1 &&
                        world.Get(c + r.gate * 6 + side * 2 + glm::ivec3(0, 3, 0)).type == BlockType::LAMP;
                marketsRight += right;
            }
            else {
                if (mines >= 4) continue;
                mines++;
                right = right && world.Get({ c.x, r.bottom, c.z }).IsAir() && world.Get({ c.x - 1, r.bottom, c.z }).type == BlockType::LADDER;
                for (const auto& [d, length] : r.tunnels) right = right && world.Get(glm::ivec3(c.x, r.bottom + 1, c.z) + d * (2 + length / 2)).IsAir();
                minesRight += right;
                if (r.deepShaft) {
                    deepShafts++;
                    deepRight += world.Get({ c.x, r.bottom - 1, c.z }).IsAir() && world.Get({ c.x - 1, (r.bottom + r.deepBottom) / 2, c.z }).type == BlockType::LADDER &&
                                 world.Get({ c.x, r.deepBottom + 1, c.z }).IsAir() && r.deepBottom < TerrainGenerator::kDeepTop;
                }
                if (r.forge) {
                    forges++;
                    const glm::ivec3 room = r.site.spawns.front();
                    forgesRight += world.Get(room + glm::ivec3(0, 1, 0)).IsAir() && world.Get(r.forgeChest).type == BlockType::CHEST && r.site.golems == 1 &&
                                   world.Get(room + glm::ivec3(0, -1, 0)).type == BlockType::FIRE_BRICK;
                }
            }
        }
    Check(camps > 0 && campsRight == camps, "dwarf camps have their fire, chest and palisade (" + std::to_string(campsRight) + " of " + std::to_string(camps) + ")");
    Check(strongholdsRight == strongholds, "dwarf strongholds have an open hall, a throne, a vault, a trap and a stairway up (" + std::to_string(strongholdsRight) + " of " +
                                               std::to_string(strongholds) + ")");
    Check(deepRight == deepShafts, "deep mines' shafts go on down, laddered, to a chamber in the Deep (" + std::to_string(deepRight) + " of " + std::to_string(deepShafts) + ")");
    Check(forgesRight == forges, "mines' forges have their fire-brick room, golem and chest (" + std::to_string(forgesRight) + " of " + std::to_string(forges) + ")");
    Check(marketsRight == markets, "dwarf markets have their fire and lamps by the gate, and no loot (" + std::to_string(marketsRight) + " of " + std::to_string(markets) + ")");
    Check(mines > 0 && minesRight == mines, "dwarf mines have an open shaft with a ladder, tunnels and chests (" + std::to_string(minesRight) + " of " + std::to_string(mines) + ")");

    // Copper in the rock, from a few blocks down.
    {
        TerrainGenerator rock(TerrainGenerator::Type::Normal);
        long copper = 0;
        for (int cx = -2; cx < 2; cx++)
            for (int cz = -2; cz < 2; cz++) {
                TerrainGenerator::ColumnHeights h;
                rock.ComputeColumnHeights(cx, cz, h);
                for (int cy = -3; cy <= 0; cy++) {
                    TerrainGenerator::Blocks blocks;
                    rock.GenerateChunk({ cx, cy, cz }, h, blocks);
                    for (const Block& b : blocks) copper += b.type == BlockType::COPPER_ORE;
                }
            }
        Check(copper > 100, "copper ore runs through the rock (" + std::to_string(copper) + " blocks in 64 chunks)");
    }
    // Limestone in beds, a few blocks down and no deeper than 48.
    {
        TerrainGenerator rock(TerrainGenerator::Type::Normal);
        long limestone = 0, tooDeep = 0, gravel = 0;
        for (int cx = -2; cx < 2; cx++)
            for (int cz = -2; cz < 2; cz++) {
                TerrainGenerator::ColumnHeights h;
                rock.ComputeColumnHeights(cx, cz, h);
                for (int cy = -6; cy <= 0; cy++) {
                    TerrainGenerator::Blocks blocks;
                    rock.GenerateChunk({ cx, cy, cz }, h, blocks);
                    for (int i = 0; i < static_cast<int>(blocks.size()); i++) {
                        gravel += blocks[i].type == BlockType::GRAVEL;
                        if (blocks[i].type != BlockType::LIMESTONE) continue;
                        limestone++;
                        if (cy * TerrainGenerator::S < -80) tooDeep++;
                    }
                }
            }
        Check(limestone > 500 && tooDeep == 0, "limestone lies in beds in the rock (" + std::to_string(limestone) + " blocks), none far down");
        Check(gravel > 200, "and gravel in pockets under the topsoil (" + std::to_string(gravel) + " blocks)");
    }
    // Cave floors slope: stone with air over it has a shaped top, not all of it flat.
    {
        TerrainGenerator rock(TerrainGenerator::Type::Normal);
        long floors = 0, sloped = 0;
        for (int cx = -3; cx < 3; cx++)
            for (int cz = -3; cz < 3; cz++) {
                TerrainGenerator::ColumnHeights h;
                rock.ComputeColumnHeights(cx, cz, h);
                for (int cy = -6; cy <= -1; cy++) {
                    TerrainGenerator::Blocks blocks;
                    rock.GenerateChunk({ cx, cy, cz }, h, blocks);
                    constexpr int S = TerrainGenerator::S;
                    for (int x = 0; x < S; x++)
                        for (int z = 0; z < S; z++)
                            for (int y = 0; y + 1 < S; y++) {
                                const Block& b = blocks[x + S * (y + S * z)];
                                if (b.type != BlockType::STONE || !blocks[x + S * (y + 1 + S * z)].IsAir()) continue;
                                floors++;
                                sloped += !b.IsFullBlock();
                            }
                }
            }
        Check(floors > 200 && sloped * 2 > floors, "cave floors are sloped stone, not flat blocks (" + std::to_string(sloped) + " of " + std::to_string(floors) + ")");
    }
    // Cave rock follows the cave: floors, ceilings and walls are shaped from the cave's own
    // field, so shaped rock facing the same way meets its neighbours at the corners they
    // share (smooth, not bumped up and down by a noise of its own).
    {
        TerrainGenerator rock(TerrainGenerator::Type::Normal);
        long shared = 0, agree = 0, ceilings = 0, walls = 0;
        constexpr int kCorner[4][2] = { { 0, 1 }, { 1, 0 }, { 0, 0 }, { 1, 1 } };
        for (int cx = -3; cx < 3; cx++)
            for (int cz = -3; cz < 3; cz++) {
                TerrainGenerator::ColumnHeights h;
                rock.ComputeColumnHeights(cx, cz, h);
                for (int cy = -6; cy <= -1; cy++) {
                    TerrainGenerator::Blocks blocks;
                    rock.GenerateChunk({ cx, cy, cz }, h, blocks);
                    constexpr int S = TerrainGenerator::S;
                    std::map<std::tuple<int, int, int, int>, std::vector<int>> corners; // (where, which way) -> heights there
                    for (int x = 0; x < S; x++)
                        for (int y = 0; y < S; y++)
                            for (int z = 0; z < S; z++) {
                                const Block& b = blocks[x + S * (y + S * z)];
                                if ((b.type != BlockType::STONE && b.type != BlockType::DEEPSTONE) || !b.IsPartial()) continue;
                                const int dir = b.ShapeDir();
                                ceilings += dir == 3, walls += dir != 0 && dir != 3;
                                for (int e = 0; e < 4; e++) {
                                    const glm::ivec3 tip = glm::ivec3(x, y, z) + glm::ivec3(glm::round(Block::ShapeToCell(dir, glm::vec3(kCorner[e][0], 1.0f, kCorner[e][1]))));
                                    corners[{ tip.x, tip.y, tip.z, dir }].push_back(b.edgeData.GetTopY(e));
                                }
                            }
                    for (const auto& [where, heights] : corners)
                        for (size_t i = 1; i < heights.size(); i++) shared++, agree += heights[i] == heights[0];
                }
            }
        Check(ceilings > 100 && walls > 100 && shared > 500 && agree * 100 >= shared * 95,
              "cave floors, ceilings (" + std::to_string(ceilings) + ") and walls (" + std::to_string(walls) + ") meet their neighbours at shared corners (" +
                  std::to_string(agree) + " of " + std::to_string(shared) + ")");
    }
    // Cave rock doesn't hang out over open space: no floor or wall leans out over the
    // rock under it, nor sticks out as a ledge over the wall below. (A ceiling hangs over
    // the cave, as it must, and rock over open cave or over a wall under a ceiling.)
    {
        TerrainGenerator rock(TerrainGenerator::Type::Normal);
        long points = 0, leaning = 0, lips = 0;
        auto stone = [](const Block& b) { return b.type == BlockType::STONE || b.type == BlockType::DEEPSTONE; };
        auto solid = [](const Block& b, float x, float y, float z) {
            if (b.IsFullBlock()) return true;
            if (!b.IsPartial()) return false;
            constexpr float d = 0.01f;
            return b.ShapeOverlaps({ x - d, y - d, z - d }, { x + d, y + d, z + d });
        };
        for (int cx = -3; cx < 3; cx++)
            for (int cz = -3; cz < 3; cz++) {
                TerrainGenerator::ColumnHeights h;
                rock.ComputeColumnHeights(cx, cz, h);
                for (int cy = -6; cy <= -1; cy++) {
                    TerrainGenerator::Blocks blocks;
                    rock.GenerateChunk({ cx, cy, cz }, h, blocks);
                    constexpr int S = TerrainGenerator::S;
                    for (int x = 0; x < S; x++)
                        for (int y = 1; y < S; y++)
                            for (int z = 0; z < S; z++) {
                                const Block& b = blocks[x + S * (y + S * z)];
                                const Block& under = blocks[x + S * (y - 1 + S * z)];
                                if (!stone(b) || (b.IsPartial() && b.ShapeDir() == 3)) continue;
                                if (!b.IsPartial() && !under.IsPartial()) continue;
                                for (int i = 0; i < 4; i++)
                                    for (int k = 0; k < 4; k++) {
                                        const float fx = (i + 0.5f) / 4, fz = (k + 0.5f) / 4;
                                        float lowest = -1.0f;
                                        for (int s = 0; s < 16 && lowest < 0.0f; s++)
                                            if (solid(b, fx, (s + 0.5f) / 16, fz)) lowest = (s + 0.5f) / 16;
                                        if (lowest < 0.0f) continue;
                                        points++;
                                        if (lowest > 0.1f) {
                                            // (Unless it curves out into a ceiling over the cave it faces.)
                                            const glm::ivec3 over = glm::ivec3(x, y + 1, z) + glm::ivec3(glm::round(Block::ShapeVectorToCell(b.ShapeDir(), { 0.0f, 1.0f, 0.0f })));
                                            const bool inside = over.x >= 0 && over.y >= 0 && over.z >= 0 && over.x < S && over.y < S && over.z < S;
                                            if (inside && blocks[over.x + S * (over.y + S * over.z)].IsAir()) leaning++;
                                            continue;
                                        }
                                        if (!under.IsPartial() || !stone(under) || !solid(b, fx, 0.01f, fz)) continue;
                                        // (A ledge: open beyond it the way the wall under it faces, and
                                        // sticking out a pixel or more past the wall's top.)
                                        const glm::vec3 out = Block::ShapeVectorToCell(under.ShapeDir(), { 0.0f, 1.0f, 0.0f });
                                        const float bx = std::clamp(fx - out.x / 16, 0.005f, 0.995f), bz = std::clamp(fz - out.z / 16, 0.005f, 0.995f);
                                        if (solid(under, bx, 0.99f, bz)) continue;
                                        const glm::ivec3 beyond = glm::ivec3(x, y, z) + glm::ivec3(glm::round(out));
                                        if (beyond.x < 0 || beyond.y < 0 || beyond.z < 0 || beyond.x >= S || beyond.y >= S || beyond.z >= S) continue;
                                        if (!blocks[beyond.x + S * (beyond.y + S * beyond.z)].IsAir()) continue;
                                        lips++;
                                    }
                            }
                }
            }
        Check(leaning + lips <= points / 1000, "cave rock doesn't hang out over the rock under it (" + std::to_string(leaning) + " leaning out, " +
                                                 std::to_string(lips) + " sticking out over a slope, of " + std::to_string(points) + " points)");
    }
    // A natural slope's flat face (its base, the side opposite its top) is always against a
    // full block, so none of it hangs over open space or another slope: in caves and on the
    // land, within a chunk and across chunks' borders.
    {
        TerrainGenerator rock(TerrainGenerator::Type::Normal);
        constexpr int S = TerrainGenerator::S;
        std::map<std::tuple<int, int, int>, TerrainGenerator::Blocks> chunks;
        for (int cx = -3; cx < 3; cx++)
            for (int cz = -3; cz < 3; cz++) {
                TerrainGenerator::ColumnHeights h;
                rock.ComputeColumnHeights(cx, cz, h);
                for (int cy = -6; cy <= 6; cy++) rock.GenerateChunk({ cx, cy, cz }, h, chunks[{ cx, cy, cz }]);
            }
        using Coords::FloorDiv;
        auto at = [&](const glm::ivec3& w) -> const Block* {
            const auto it = chunks.find({ FloorDiv(w.x, S), FloorDiv(w.y, S), FloorDiv(w.z, S) });
            if (it == chunks.end()) return nullptr;
            const glm::ivec3 l = w - glm::ivec3(FloorDiv(w.x, S), FloorDiv(w.y, S), FloorDiv(w.z, S)) * S;
            return &it->second[l.x + S * (l.y + S * l.z)];
        };
        long caveSlopes = 0, caveLoose = 0, landSlopes = 0, landLoose = 0, borders = 0;
        for (const auto& [where, blocks] : chunks) {
            const glm::ivec3 origin = glm::ivec3(std::get<0>(where), std::get<1>(where), std::get<2>(where)) * S;
            for (int x = 0; x < S; x++)
                for (int y = 0; y < S; y++)
                    for (int z = 0; z < S; z++) {
                        const Block& b = blocks[x + S * (y + S * z)];
                        if (!b.IsPartial() || b.IsStepped()) continue;
                        const glm::ivec3 local(x, y, z);
                        const glm::ivec3 base = origin + local - glm::ivec3(glm::round(Block::ShapeVectorToCell(b.ShapeDir(), { 0.0f, 1.0f, 0.0f })));
                        const Block* under = at(base);
                        if (!under) continue;
                        const bool cave = b.type == BlockType::STONE || b.type == BlockType::DEEPSTONE;
                        const bool loose = !under->IsFullBlock();
                        (cave ? caveSlopes : landSlopes)++;
                        (cave ? caveLoose : landLoose) += loose;
                        borders += loose && glm::any(glm::notEqual(glm::ivec3(FloorDiv(base.x, S), FloorDiv(base.y, S), FloorDiv(base.z, S)) * S, origin));
                    }
        }
        Check(caveSlopes > 1000 && caveLoose == 0, "cave slopes' flat faces are all against full blocks (" + std::to_string(caveLoose) + " of " + std::to_string(caveSlopes) + " not)");
        Check(landSlopes > 1000 && landLoose == 0, "and the land's slopes' too (" + std::to_string(landLoose) + " of " + std::to_string(landSlopes) + " not)");
    }
    // Below the lava level: deepstone, not stone, and caves much rarer.
    {
        long above = 0, below = 0, samples = 0;
        for (int x = -200; x < 200; x += 3)
            for (int z = -200; z < 200; z += 3)
                for (int dy = 0; dy < 30; dy += 3) {
                    above += Caves::IsCave(x, -90 + dy, z, 0);  // (well under the ground, over the lava level)
                    below += Caves::IsCave(x, -200 + dy, z, 0); // (under the Deep)
                    samples++;
                }
        Check(below * 3 < above, "caves are far rarer under the lava level (" + std::to_string(below) + " cave samples against " + std::to_string(above) + " above it, of " + std::to_string(samples) + ")");
        TerrainGenerator rock(TerrainGenerator::Type::Normal);
        long deepstone = 0, stoneBelow = 0, deepstoneAbove = 0;
        for (int cx = -1; cx < 1; cx++)
            for (int cz = -1; cz < 1; cz++) {
                TerrainGenerator::ColumnHeights h;
                rock.ComputeColumnHeights(cx, cz, h);
                for (const int cy : { -5, -12 }) { // (-80..-65 over it; -192..-177 under the Deep)
                    TerrainGenerator::Blocks blocks;
                    rock.GenerateChunk({ cx, cy, cz }, h, blocks);
                    for (const Block& b : blocks) {
                        if (cy == -12) deepstone += b.type == BlockType::DEEPSTONE, stoneBelow += b.type == BlockType::STONE;
                        else deepstoneAbove += b.type == BlockType::DEEPSTONE;
                    }
                }
            }
        Check(deepstone > 1000 && stoneBelow == 0 && deepstoneAbove == 0,
              "under the lava level the rock is deepstone (" + std::to_string(deepstone) + " blocks, " + std::to_string(stoneBelow) + " stone), and none above it");
    }

    // The Deep: open caverns with fungus, magma and gold ore far down, and no gold above.
    {
        TerrainGenerator deep(TerrainGenerator::Type::Normal);
        long open = 0, fungus = 0, magma = 0, goldDeep = 0, goldHigh = 0, cells = 0, lavaDeep = 0, lavaHigh = 0;
        for (int cx = -3; cx < 3; cx++)
            for (int cz = -3; cz < 3; cz++) {
                TerrainGenerator::ColumnHeights h;
                deep.ComputeColumnHeights(cx, cz, h);
                for (int cy = -10; cy <= -2; cy++) {
                    TerrainGenerator::Blocks blocks;
                    deep.GenerateChunk({ cx, cy, cz }, h, blocks);
                    for (const Block& b : blocks) {
                        const bool inDeep = cy <= -7;
                        if (inDeep) cells++, open += b.IsAir(), fungus += b.type == BlockType::GLOW_FUNGUS, magma += b.type == BlockType::MAGMA;
                        (inDeep ? goldDeep : goldHigh) += b.type == BlockType::GOLD_ORE;
                        (inDeep ? lavaDeep : lavaHigh) += b.type == BlockType::LAVA;
                    }
                }
            }
        Check(open > cells / 20 && open < cells / 5 && fungus > 50 && magma > 20 && goldDeep > 50 && goldHigh == 0,
              "the Deep has caverns (" + std::to_string(100 * open / std::max(1L, cells)) + "% open), glow fungus, magma and gold ore, and no gold above it");
        Check(lavaDeep > 20 && lavaHigh == 0, "and pools of lava on its floors (" + std::to_string(lavaDeep) + " cells), none above it");
    }

    std::cout << (failures ? "BIOME TEST FAILED (" : "ALL BIOME CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
