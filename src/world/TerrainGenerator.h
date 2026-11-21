#pragma once

#include <array>
#include <climits>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <glm/glm.hpp>

#include "world/Block.h"
#include "world/Chunk.h"

// Deterministic world generation: a sloped heightmap surface, caves carved with 3D
// noise, surface features (trees, bushes, boulders, grass and flowers), then
// underground dungeons (see Dungeons).
// Stateless and safe to call from any thread.
//
// Invariants relied on by sky lighting (see LightPropagator): above each column's
// surface row, generated terrain never has an opaque block hanging over open air,
// and the only thing between the ground and the sky is water up to sea level.
// Caves only remove blocks (and never flood), trunks and boulders are solid down to
// the ground, and leaves and plants let light through. Dungeons are deep underground;
// their entrance shafts only remove blocks below the surface (clearing anything above
// the opening) and their brick rings stand on the ground.
// Biomes, from a column's climate (see TerrainGenerator::ClimateAt): temperature
// (falling with height) against humidity.
enum class Biome : uint8_t {
    Plains,     // temperate and dry: open grass, flowers, the odd tree
    Forest,     // temperate: oak woods
    Swamp,      // temperate and very wet: lowland at sea level, standing water, lush grass
    Desert,     // hot and dry: sand, cacti, dead bushes
    Savanna,    // hot: dry grass, flat-topped trees far apart
    Jungle,     // hot and wet: lush grass, tall trees close together, thick undergrowth
    Taiga,      // cold: pine forest
    SnowyTaiga, // freezing and wet: pines in snow, frozen seas
    Tundra,     // freezing and dry: snowy open ground, frozen seas
    Count
};
const char* BiomeName(Biome biome);
// Whether seas freeze over (their top layer is ice).
inline bool IsFrozen(Biome b) { return b == Biome::SnowyTaiga || b == Biome::Tundra; }

class TerrainGenerator {
public:
    // A column's climate, each roughly -1..1. Temperature falls with height.
    struct Climate {
        float temperature, humidity;
    };

    // Normal: the generated landscape. Empty: a flat plain for testing, grass at row -1
    // (standing height 0) over dirt over stone, with nothing else: no hills, caves,
    // water, trees or plants.
    enum class Type : uint8_t { Normal = 0, Empty = 1 };
    static constexpr Type kLastType = Type::Empty;

    explicit TerrainGenerator(Type type = Type::Normal) : type(type) {}
    Type GetType() const { return type; }
    // Before the generator is first used (the server sets it from the save).
    void SetType(Type t) { type = t; }

    static constexpr int S = Chunk::CHUNK_SIZE;
    // Air below this row (above the ground) is filled with water.
    static constexpr int kSeaLevel = -3;
    // Columns whose surface is at most one row above sea level are sand (beaches and
    // sea floors), and nothing grows there.
    static constexpr bool IsSandyRow(int surfaceRow) { return surfaceRow <= kSeaLevel; }
    // Mountains: bare rock from here up (and on steep slopes), snow on the peaks.
    // Trees and plants keep below the rock line.
    static constexpr int kRockLine = 44;
    static constexpr int kSnowLine = 70;
    using Blocks = std::array<Block, Chunk::VOLUME>;

    // Heights at the (S + 1)^2 block corners of one chunk column. Identical for every
    // chunk stacked in the column, so World caches it.
    using ColumnHeights = std::array<float, (S + 1) * (S + 1)>;
    // Surface row of each column in a chunk, indexed [x * S + z].
    using SurfaceRows = std::array<int, S * S>;

    // The surface of one block column: quantised corner heights (in EdgeData order)
    // and the surface row, i.e. the row holding the sloped grass block. Everything
    // below the surface row is solid ground, before caves are carved.
    struct Column {
        float corners[4];
        int surfaceRow;

        // Edge data for the sloped block at `row`, and whether it's a valid block.
        EdgeData EdgesAt(int row) const;
        bool HasSlopedTop() const { return EdgesAt(surfaceRow).IsValid(); }
        // Height of the visible ground at the column's centre.
        float GroundHeightAtCentre() const;
        // First row above the ground that's entirely open (water fills from here).
        int FirstOpenRow() const { return HasSlopedTop() ? surfaceRow + 1 : surfaceRow; }
        // Sandy beaches and sea floors instead of grass.
        bool IsSandy() const { return IsSandyRow(surfaceRow); }
        // Sky light reaching the cell just above `row` in untouched terrain, coming
        // straight down: 0 under the ground, reduced by any water above (up to
        // `waterLevel`: the sea's, or a mountain lake's).
        uint8_t SkyLightAbove(int row, int waterLevel = kSeaLevel) const;
        // What the column is made of at the top (the biome's grass, sand, rock or snow)
        // and just under it (dirt, sand or rock).
        BlockType TopType(Biome biome) const;
        BlockType UnderType(Biome biome) const;
        // Whether grass and flowers grow here (any grass, not sand, rock or snow).
        bool IsGrassy(Biome biome) const { return IsGrassType(TopType(biome)); }
    };

    float GetHeight(int x, int z) const;

    // Lakes in the hills and mountains: on a grid of kLakeCell blocks, most cells
    // above the lowlands hold a round lake, kLakeMinRadius..kLakeMaxRadius across,
    // in a bowl carved into the land (an exact bowl under the water; on the uphill
    // side the shore eases up into the slope) with its rim raised just above the
    // water all round, so it's always contained. Its level is near the lowest ground
    // around it. The row water fills up to (exclusive) in this column, or INT_MIN.
    int LakeLevelAt(int x, int z) const;
    // Where a lake overflows: a spring (BlockType::SPRING) in the ground just outside
    // its rim on its lowest side, where the land falls away below its level. The
    // springs in a chunk's columns, or none.
    std::vector<glm::ivec2> SpringsIn(int chunkX, int chunkZ) const;
    // Rivers: from each spring a channel runs downhill (following the land's fall,
    // turning gently), kRiverHalfWidth either side of its line, its bed kRiverDepth
    // below the ground and never rising, with banks easing up into the land. Its bed is
    // sand over dirt, it's generated full of water a block deep, and it ends, after at
    // most kMaxRiverLength blocks, where it reaches the sea or would have to cut deeper
    // than kRiverMaxCut, at a sinkhole (BlockType::SINKHOLE, a natural drain) in its
    // bed that takes the water away. The drains in a chunk's columns, or none.
    std::vector<glm::ivec2> DrainsIn(int chunkX, int chunkZ) const;
    // Oil: pockets of oil shale in a band deep underground (kOilBottom..kOilTop), in
    // broad oil fields covering a part of the world. A function of position alone.
    static bool OilAt(int x, int y, int z);
    static constexpr int kOilTop = -30, kOilBottom = -80;
    // The Deep: a band of huge caverns far down, lit by glowing fungus, with magma
    // and gold ore (and richer iron) in its rock.
    static constexpr int kDeepTop = -100, kDeepBottom = -160;
    // Whether a column's top is a tar seep (one in 40 of those over oil, on dry land).
    bool TarSeepAt(int x, int z) const;
    // The nearest column within `range` with oil under it (sampled every 4 blocks).
    std::optional<glm::ivec2> OilNear(int x, int z, int range) const;
    // Whether a column's middle is in a river's channel (within `margin` more of it).
    bool InRiver(int x, int z, float margin = 0.0f) const;
    static constexpr float kRiverHalfWidth = 1.6f, kRiverBanks = 2.5f, kRiverDepth = 2.0f, kRiverMaxCut = 7.0f;
    static constexpr int kMaxRiverLength = 160;
    // Water fills open cells below this row: the sea's level, or a lake's.
    int WaterLevelAt(int x, int z) const { return std::max(static_cast<int>(kSeaLevel), LakeLevelAt(x, z)); }
    static constexpr int kLakeCell = 128;
    static constexpr float kLakeMinRadius = 14.0f, kLakeMaxRadius = 34.0f;
    void ComputeColumnHeights(int chunkX, int chunkZ, ColumnHeights& out) const;

    static Column ColumnFrom(const ColumnHeights& heights, int lx, int lz);
    // Same as ColumnFrom, for any world column (computes the heights directly).
    Column ColumnAt(int wx, int wz) const;
    static void ComputeSurfaceRows(const ColumnHeights& heights, SurfaceRows& out);

    void GenerateChunk(const glm::ivec3& chunkCoords, const ColumnHeights& heights, Blocks& out) const;

    // Climate and biome of a world column (`surfaceRow` for the altitude; the second
    // form works it out).
    Climate ClimateAt(int wx, int wz, int surfaceRow) const;
    Biome BiomeAt(int wx, int wz, int surfaceRow) const { return Classify(ClimateAt(wx, wz, surfaceRow)); }
    Biome BiomeAt(int wx, int wz) const { return BiomeAt(wx, wz, ColumnAt(wx, wz).surfaceRow); }
    static Biome Classify(const Climate& climate);

private:
    static constexpr int OCTAVES = 8;
    static constexpr float SIZE = 120.0f;
    static constexpr float AMPLITUDE = 20.0f;
    // Mountain ranges on top of the rolling hills (see GetHeight).
    static constexpr float kMountainHeight = 150.0f;

    struct Lake {
        bool exists = false;
        glm::vec2 centre{ 0.0f };
        float radius = 0.0f, depth = 0.0f;
        int level = 0;
        bool hasSpring = false;
        glm::ivec2 spring{ 0 }; // the column of its spring
        // Its river: points a block apart from the spring (x, z, and the bed's height),
        // ending at its drain.
        std::vector<glm::vec3> river;
        glm::ivec2 drain{ 0 };
    };
    // The river points near a chunk's columns (from the lakes around), worked out once
    // and kept (any thread).
    std::shared_ptr<const std::vector<glm::vec3>> RiverPointsNear(int chunkX, int chunkZ) const;
    // The nearest river point's distance from (x, z) and its bed height (false: none near).
    bool NearestRiver(float x, float z, float& distance, float& bed) const;
    struct RiverCache {
        std::mutex mutex;
        std::unordered_map<int64_t, std::shared_ptr<const std::vector<glm::vec3>>> near;
    };
    std::shared_ptr<RiverCache> riverCache = std::make_shared<RiverCache>();
    // The lake in grid cell (cx, cz), worked out once and kept (any thread).
    const Lake& LakeIn(int cx, int cz) const;
    // The land before lakes are carved into it, and with them (before rivers).
    float GetBaseHeight(int x, int z) const;
    float GetLakeHeight(int x, int z) const;
    struct LakeCache {
        std::mutex mutex;
        std::unordered_map<int64_t, Lake> lakes;
    };
    std::shared_ptr<LakeCache> lakeCache = std::make_shared<LakeCache>();

    void PlacePlants(const glm::ivec3& chunkCoords, const ColumnHeights& heights, const SurfaceRows& rows, Blocks& out) const;
    float GetHillHeight(int x, int z) const;
    // Climate before altitude: what shapes the land (swamps sink to sea level).
    Climate BaseClimate(int x, int z) const;
    float GetMountainHeight(int x, int z) const;

    Type type;
};
