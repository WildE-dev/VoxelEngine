#pragma once

#include <array>

#include <glm/glm.hpp>

#include "world/Block.h"
#include "world/Chunk.h"

// Deterministic world generation: a sloped heightmap surface, caves carved with 3D
// noise, then surface features (trees, bushes, boulders, grass and flowers).
// Stateless and safe to call from any thread.
//
// Invariants relied on by sky lighting (see LightPropagator): above each column's
// surface row, generated terrain never has an opaque block hanging over open air,
// and the only thing between the ground and the sky is water up to sea level.
// Caves only remove blocks (and never flood), trunks and boulders are solid down to
// the ground, and leaves and plants let light through.
class TerrainGenerator {
public:
    static constexpr int S = Chunk::CHUNK_SIZE;
    // Air below this row (above the ground) is filled with water.
    static constexpr int kSeaLevel = -3;
    // Columns whose surface is at most one row above sea level are sand (beaches and
    // sea floors), and nothing grows there.
    static constexpr bool IsSandyRow(int surfaceRow) { return surfaceRow <= kSeaLevel; }
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
    };

    float GetHeight(int x, int z) const;
    void ComputeColumnHeights(int chunkX, int chunkZ, ColumnHeights& out) const;

    static Column ColumnFrom(const ColumnHeights& heights, int lx, int lz);
    // Same as ColumnFrom, for any world column (computes the heights directly).
    Column ColumnAt(int wx, int wz) const;
    static void ComputeSurfaceRows(const ColumnHeights& heights, SurfaceRows& out);

    void GenerateChunk(const glm::ivec3& chunkCoords, const ColumnHeights& heights, Blocks& out) const;

private:
    static constexpr int OCTAVES = 8;
    static constexpr float SIZE = 120.0f;
    static constexpr float AMPLITUDE = 20.0f;

    void PlacePlants(const glm::ivec3& chunkCoords, const ColumnHeights& heights, Blocks& out) const;
};
