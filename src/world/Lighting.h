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
//
// Space above the loaded region is treated as open sky. Unloaded chunks are always
// freshly generated terrain (edits don't survive unloading), and the generator
// makes a pure heightmap with no overhangs, so that assumption holds.
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
    // Seeds the chunk's light and links it with its loaded neighbours.
    void OnChunkLoaded(Chunk& chunk);

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
    std::array<ChannelQueues, 2> queues;
};
