#include "world/Lighting.h"

#include "world/Chunk.h"

namespace {

constexpr int S = Chunk::CHUNK_SIZE;

const glm::ivec3 kNeighbours[kFaceCount] = {
    { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
};
const glm::ivec3 kDown(0, -1, 0);
const glm::ivec3 kUp(0, 1, 0);

constexpr LightChannel kChannels[] = { LightChannel::Block, LightChannel::Sky };

uint8_t Emission(LightChannel channel, const Block& block) {
    return channel == LightChannel::Block ? block.GetLightEmission() : 0;
}

// Level a transparent neighbour in direction `dir` receives from a cell at `level`.
// Water absorbs light: entering it costs 2 levels instead of 1, and full strength
// sky light doesn't pass straight down into it unchanged.
uint8_t Spread(LightChannel channel, uint8_t level, const glm::ivec3& dir, bool intoWater) {
    if (intoWater) return level > 2 ? level - 2 : 0;
    if (channel == LightChannel::Sky && level == kMaxLight && dir == kDown) return kMaxLight;
    return level > 0 ? level - 1 : 0;
}

int Index(int x, int y, int z) { return x + S * (y + S * z); }

} // namespace

bool LightPropagator::Resolve(const glm::ivec3& worldPos, Cell& cell) {
    const glm::ivec3 coords = Coords::ChunkOf(worldPos);
    if (!cacheValid || coords != cachedCoords) {
        cachedCoords = coords;
        cachedChunk = lookup(coords);
        cacheValid = true;
    }
    if (!cachedChunk) return false;
    cell = { cachedChunk, Coords::LocalOf(worldPos) };
    return true;
}

uint8_t LightPropagator::SkyFromUnloadedAbove(const glm::ivec3& worldPos, const Cell& cell, const Block& block) {
    if (cell.local.y != S - 1 || block.BlocksLight()) return 0;
    if (lookup(Coords::ChunkOf(worldPos) + kUp)) return 0; // the chunk above is loaded: it's a normal neighbour
    return Spread(LightChannel::Sky, cell.chunk->GetSkyFromAbove(cell.local.x, cell.local.z), kDown, block.HasWater());
}

uint8_t LightPropagator::GetLight(LightChannel channel, const Cell& cell) const {
    return cell.chunk->GetLight(channel, cell.local.x, cell.local.y, cell.local.z);
}

void LightPropagator::SetLight(LightChannel channel, const glm::ivec3& worldPos, const Cell& cell, uint8_t level, ChunkSet& dirty) {
    cell.chunk->SetLight(channel, cell.local.x, cell.local.y, cell.local.z, level);
    Coords::ForEachChunkTouching(worldPos, [&](const glm::ivec3& c) { dirty.insert(c); });
}

void LightPropagator::OnBlockChanged(const glm::ivec3& worldPos) {
    changedBlocks.push_back(worldPos);
}

void LightPropagator::OnChunkLoaded(Chunk& chunk, const std::array<uint8_t, Chunk::CHUNK_SIZE * Chunk::CHUNK_SIZE>* skyFromAbove) {
    cacheValid = false;
    const glm::ivec3 coords = chunk.GetCoords();
    const glm::ivec3 origin = coords * S;

    auto blocksPtr = std::make_unique<std::array<Block, Chunk::VOLUME>>();
    auto& blocks = *blocksPtr;
    chunk.CopyBlocks(blocks);

    // --- Block light: emitters inside the chunk ---
    for (int z = 0; z < S; z++)
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++)
                if (uint8_t emission = blocks[Index(x, y, z)].GetLightEmission()) {
                    chunk.SetLight(LightChannel::Block, x, y, z, emission);
                    QueuesFor(LightChannel::Block).addition.push_back(origin + glm::ivec3(x, y, z));
                }

    if (skyFromAbove) chunk.SetSkyFromAbove(*skyFromAbove);

    // --- Sky light: straight down each column from the cell above ---
    // That cell's level comes from the chunk above if it's loaded, otherwise from the
    // generator's estimate (exact for the straight-down path, so never too bright).
    Chunk* above = lookup(coords + kUp);
    bool anyDim = false;
    for (int z = 0; z < S; z++) {
        for (int x = 0; x < S; x++) {
            uint8_t level = above ? above->GetLight(LightChannel::Sky, x, 0, z)
                                  : (skyFromAbove ? (*skyFromAbove)[x * S + z] : kMaxLight);
            for (int y = S - 1; y >= 0; y--) {
                const Block& block = blocks[Index(x, y, z)];
                level = block.BlocksLight() ? 0 : Spread(LightChannel::Sky, level, kDown, block.HasWater());
                chunk.SetLight(LightChannel::Sky, x, y, z, level);
                if (!block.BlocksLight() && level < kMaxLight) anyDim = true;
            }
        }
    }
    // Dim cells (shadows, water, caves) may get more light sideways from brighter ones.
    if (anyDim) {
        for (int z = 0; z < S; z++)
            for (int y = 0; y < S; y++)
                for (int x = 0; x < S; x++)
                    if (chunk.GetLight(LightChannel::Sky, x, y, z) > 1)
                        QueuesFor(LightChannel::Sky).addition.push_back(origin + glm::ivec3(x, y, z));
    }

    // --- Link with loaded neighbours across each shared face ---
    // Only cells where light would actually flow are queued, which keeps chunk
    // loading cheap: in untouched terrain both sides already agree.
    for (int face = 0; face < kFaceCount; face++) {
        const glm::ivec3 dir = kNeighbours[face];
        Chunk* neighbour = lookup(coords + dir);
        if (!neighbour) continue;

        const int axis = kFaceAxis[face];
        const int ta = kFaceTangents[axis][0], tb = kFaceTangents[axis][1];
        for (int a = 0; a < S; a++) {
            for (int b = 0; b < S; b++) {
                glm::ivec3 ours(0), theirs(0);
                ours[axis] = dir[axis] > 0 ? S - 1 : 0;
                theirs[axis] = dir[axis] > 0 ? 0 : S - 1;
                ours[ta] = theirs[ta] = a;
                ours[tb] = theirs[tb] = b;

                const Block& ourBlock = blocks[Index(ours.x, ours.y, ours.z)];
                const bool oursOpen = !ourBlock.BlocksLight();
                for (LightChannel channel : kChannels) {
                    uint8_t ourLevel = chunk.GetLight(channel, ours.x, ours.y, ours.z);
                    uint8_t theirLevel = neighbour->GetLight(channel, theirs.x, theirs.y, theirs.z);
                    // Ours -> theirs (an upper bound; the addition pass checks their block).
                    if (Spread(channel, ourLevel, dir, false) > theirLevel)
                        QueuesFor(channel).addition.push_back(origin + ours);
                    // Theirs -> ours.
                    if (oursOpen && Spread(channel, theirLevel, -dir, ourBlock.HasWater()) > ourLevel)
                        QueuesFor(channel).addition.push_back((coords + dir) * S + theirs);
                }
            }
        }
    }
}

void LightPropagator::Propagate(ChunkSet& dirty) {
    cacheValid = false;
    Cell cell;

    // Clear the light at every changed block (it may now be opaque, or have stopped
    // emitting); the removal pass then clears everything that depended on it.
    for (const auto& pos : changedBlocks) {
        if (!Resolve(pos, cell)) continue;
        for (LightChannel channel : kChannels) {
            if (uint8_t old = GetLight(channel, cell)) {
                SetLight(channel, pos, cell, 0, dirty);
                QueuesFor(channel).removal.push_back({ pos, old });
            }
        }
    }

    for (LightChannel channel : kChannels) PropagateChannel(channel, dirty);
    changedBlocks.clear();
}

void LightPropagator::PropagateChannel(LightChannel channel, ChunkSet& dirty) {
    auto& q = QueuesFor(channel);
    Cell cell, n;

    // 1. Removal: clear light that came from removed light, collect other sources.
    while (!q.removal.empty()) {
        const Removal r = q.removal.front();
        q.removal.pop_front();
        for (const auto& d : kNeighbours) {
            const glm::ivec3 np = r.pos + d;
            if (!Resolve(np, n)) continue;
            uint8_t level = GetLight(channel, n);
            if (level == 0) continue;

            const bool dependent = level < r.level ||
                (channel == LightChannel::Sky && d == kDown && r.level == kMaxLight && level == kMaxLight);
            // (A cell that gives light of its own, glow fungus by a lamp say, goes back to
            // its own light, not stays at the brighter light it had from what's gone.)
            const uint8_t own = Emission(channel, n.chunk->GetBlock(n.local.x, n.local.y, n.local.z));
            if (dependent && level > own) {
                SetLight(channel, np, n, own, dirty);
                q.removal.push_back({ np, level });
                if (own > 0) q.addition.push_back(np);
                // It may still get light straight from unloaded space above; restore
                // that once removal is done (doing it now could loop).
                if (channel == LightChannel::Sky && n.local.y == S - 1) clearedSkySources.push_back(np);
            }
            else {
                q.addition.push_back(np); // lit by another source: re-flood from here
            }
        }
    }

    // Cells under unloaded space keep the sky light coming straight down into them.
    for (const auto& pos : clearedSkySources) {
        if (!Resolve(pos, cell)) continue;
        const Block block = cell.chunk->GetBlock(cell.local.x, cell.local.y, cell.local.z);
        const uint8_t fromAbove = SkyFromUnloadedAbove(pos, cell, block);
        if (fromAbove > GetLight(channel, cell)) {
            SetLight(channel, pos, cell, fromAbove, dirty);
            q.addition.push_back(pos);
        }
    }
    clearedSkySources.clear();

    // 2. Re-seed changed blocks: new emitters shine, newly transparent cells let
    //    neighbouring light back in.
    for (const auto& pos : changedBlocks) {
        if (!Resolve(pos, cell)) continue;
        const Block block = cell.chunk->GetBlock(cell.local.x, cell.local.y, cell.local.z);
        if (uint8_t emission = Emission(channel, block)) {
            SetLight(channel, pos, cell, emission, dirty);
            q.addition.push_back(pos);
        }
        if (block.BlocksLight()) continue;

        for (const auto& d : kNeighbours) {
            const glm::ivec3 np = pos + d;
            if (Resolve(np, n)) {
                if (GetLight(channel, n) > 1) q.addition.push_back(np);
            }
            else if (channel == LightChannel::Sky && d == kUp && Resolve(pos, cell)) {
                // Directly under unloaded space: the light coming down from it.
                if (uint8_t fromAbove = SkyFromUnloadedAbove(pos, cell, block); fromAbove > GetLight(channel, cell)) {
                    SetLight(channel, pos, cell, fromAbove, dirty);
                    q.addition.push_back(pos);
                }
            }
        }
    }

    // 3. Flood outwards.
    while (!q.addition.empty()) {
        const glm::ivec3 pos = q.addition.front();
        q.addition.pop_front();
        if (!Resolve(pos, cell)) continue;
        const uint8_t level = GetLight(channel, cell);
        if (level <= 1) continue;

        for (const auto& d : kNeighbours) {
            const glm::ivec3 np = pos + d;
            if (!Resolve(np, n)) continue;
            const uint8_t current = GetLight(channel, n);
            if (current >= Spread(channel, level, d, false)) continue; // cheap upper bound before locking the block
            const Block block = n.chunk->GetBlock(n.local.x, n.local.y, n.local.z);
            if (block.BlocksLight()) continue;
            const uint8_t spread = Spread(channel, level, d, block.HasWater());
            if (current >= spread) continue;
            SetLight(channel, np, n, spread, dirty);
            q.addition.push_back(np);
        }
    }
}
