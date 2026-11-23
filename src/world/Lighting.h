#pragma once

#include <array>
#include <deque>
#include <functional>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "world/Block.h"
#include "world/Coords.h"

class Chunk;

// Flood-fill light propagation for two channels (levels 0..kMaxLight):
//  - Block light spreads from emitters, losing one level per block.
//  - Sky light enters from above. Full strength sky light travels straight down
//    without loss; in every other direction it loses one level per block.
//  - Both lose two levels instead of one entering water, so deep water gets dark.
//
// While the chunk above isn't loaded, the sky light entering each column comes from
// the caller (ServerWorld): the generator's estimate for untouched terrain, which
// above the ground is only water up to sea level (see TerrainGenerator), zeroed in
// columns where a saved, edited chunk above has something in the way (see
// WorldStorage::SkyBlockedAbove). That estimate is never too bright; any extra light
// (cave openings, sideways through water, dug-out ground) is added once the chunks
// above load.
//
// Runs entirely on the world worker thread. The chunk lookup must only return
// loaded chunks, and the pointers it returns must stay valid for the duration of
// each call (World holds its chunk map lock shared around them).
class LightPropagator
{
public:
    using ChunkLookup = std::function<Chunk*(const glm::ivec3& chunkCoords)>;
    using ChunkSet = std::unordered_set<glm::ivec3, Coords::IVec3Hash>;

    explicit LightPropagator(ChunkLookup lookup) : lookup(std::move(lookup)) {}

    // Queue work. Nothing propagates until Propagate() is called.
    void OnBlockChanged(const glm::ivec3& worldPos);
    // Seeds the chunk's light and links it with its loaded neighbours. skyFromAbove
    // (indexed [x * CHUNK_SIZE + z]) is the sky light in the cell just above each
    // column, used while the chunk above isn't loaded; null means full sky light.
    void OnChunkLoaded(Chunk& chunk, const std::array<uint8_t, Chunk::CHUNK_SIZE * Chunk::CHUNK_SIZE>* skyFromAbove = nullptr);

    // Runs all queued work. Adds every chunk whose mesh is affected to `dirty`.
    void Propagate(ChunkSet& dirty);

private:
    struct Cell {
        Chunk* chunk = nullptr;
        glm::ivec3 local{0};
    };
    struct Removal {
        glm::ivec3 pos;
        uint8_t level;
    };
    struct ChannelQueues {
        std::deque<Removal> removal;
        std::deque<glm::ivec3> addition;
    };

    bool Resolve(const glm::ivec3& worldPos, Cell& cell);
    // Sky light a cell gets straight from unloaded space above it (top layer of a
    // chunk whose upper neighbour isn't loaded), else 0.
    uint8_t SkyFromUnloadedAbove(const glm::ivec3& worldPos, const Cell& cell, const Block& block);
    uint8_t GetLight(LightChannel channel, const Cell& cell) const;
    void SetLight(LightChannel channel, const glm::ivec3& worldPos, const Cell& cell, uint8_t level, ChunkSet& dirty);
    void PropagateChannel(LightChannel channel, ChunkSet& dirty);

    ChannelQueues& QueuesFor(LightChannel channel) { return queues[static_cast<int>(channel)]; }

    ChunkLookup lookup;

    // Single-entry cache: consecutive lookups are almost always in the same chunk.
    // Cleared at the start of every public call since pointers are only valid then.
    glm::ivec3 cachedCoords{0};
    Chunk* cachedChunk = nullptr;
    bool cacheValid = false;

    std::vector<glm::ivec3> changedBlocks;
    std::vector<glm::ivec3> clearedSkySources; // re-seeded after each removal pass
    std::array<ChannelQueues, 2> queues;
};
