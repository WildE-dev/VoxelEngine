#pragma once

#include <array>

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
        // straight down: 0 under the ground, reduced by any water above.
        uint8_t SkyLightAbove(int row) const;
        // What the column is made of at the top (the biome's grass, sand, rock or snow)
        // and just under it (dirt, sand or rock).
        BlockType TopType(Biome biome) const;
        BlockType UnderType(Biome biome) const;
        // Whether grass and flowers grow here (any grass, not sand, rock or snow).
        bool IsGrassy(Biome biome) const { return IsGrassType(TopType(biome)); }
    };

    float GetHeight(int x, int z) const;
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
    static constexpr float kMountainHeight = 90.0f;

    void PlacePlants(const glm::ivec3& chunkCoords, const ColumnHeights& heights, const SurfaceRows& rows, Blocks& out) const;
    float GetHillHeight(int x, int z) const;
    // Climate before altitude: what shapes the land (swamps sink to sea level).
    Climate BaseClimate(int x, int z) const;
    float GetMountainHeight(int x, int z) const;

    Type type;
};
