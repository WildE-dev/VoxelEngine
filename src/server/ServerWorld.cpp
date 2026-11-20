#include "server/ServerWorld.h"

#include "server/WorldStorage.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {

// Number of generate jobs the worker takes per batch before propagating light.
// Batching lets neighbouring chunks load together, so light settles across them in
// one pass.
constexpr size_t kGenerateBatchSize = 32;

} // namespace

ServerWorld::ServerWorld(const TerrainGenerator& terrain, WorldStorage* storage)
    : terrain(terrain),
      storage(storage),
      light([this](const glm::ivec3& coords) -> Chunk* {
          // Called with chunksMutex held shared (see PropagateLight).
          auto it = chunks.find(coords);
          return it != chunks.end() && it->second->IsLoaded() && !it->second->IsRemoved() ? it->second.get() : nullptr;
      }) {
    const int r = RENDER_DISTANCE;
    const int v = VERTICAL_RENDER_DISTANCE;
    for (int x = -r; x <= r; x++)
        for (int y = -v; y < v; y++)
            for (int z = -r; z <= r; z++)
                loadOffsets.emplace_back(x, y, z);

    std::sort(loadOffsets.begin(), loadOffsets.end(), [](const glm::ivec3& a, const glm::ivec3& b) {
        return a.x * a.x + a.y * a.y + a.z * a.z < b.x * b.x + b.y * b.y + b.z * b.z;
    });

    worker = std::thread(&ServerWorld::WorkerLoop, this);
}

ServerWorld::~ServerWorld() {
    water.Stop(); // uses the chunks, so it goes first
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopping = true;
    }
    queueCv.notify_all();
    if (worker.joinable()) worker.join();
    for (auto& [coords, chunk] : chunks) chunk->MarkRemoved();
}

ServerWorld::ChunkPtr ServerWorld::FindChunkUnlocked(const glm::ivec3& chunkCoords) const {
    auto it = chunks.find(chunkCoords);
    return it != chunks.end() ? it->second : nullptr;
}

ServerWorld::ChunkPtr ServerWorld::FindChunkShared(const glm::ivec3& chunkCoords) const {
    std::shared_lock<std::shared_mutex> lock(chunksMutex);
    return FindChunkUnlocked(chunkCoords);
}

std::shared_ptr<const Chunk> ServerWorld::FindChunk(const glm::ivec3& chunkCoords) const {
    return FindChunkUnlocked(chunkCoords);
}

bool ServerWorld::GetBlock(int x, int y, int z, Block& block) const {
    auto chunk = FindChunkShared(Coords::ChunkOf({ x, y, z }));
    if (!chunk || !chunk->IsLoaded()) return false;

    auto local = Coords::LocalOf({ x, y, z });
    block = chunk->GetBlock(local.x, local.y, local.z);
    return true;
}

void ServerWorld::SetBlock(int x, int y, int z, const Block& block) {
    struct PendingEdit {
        std::atomic<int>& count;
        explicit PendingEdit(std::atomic<int>& c) : count(c) { count.fetch_add(1, std::memory_order_acq_rel); }
        ~PendingEdit() { count.fetch_sub(1, std::memory_order_acq_rel); }
    } pending(pendingEdits);
    std::lock_guard<std::recursive_mutex> edit(editMutex);
    auto chunk = FindChunkShared(Coords::ChunkOf({ x, y, z }));
    // Edits to chunks that haven't generated yet would be overwritten by generation,
    // and unloaded ones are gone.
    if (!chunk || !chunk->IsLoaded() || chunk->IsRemoved()) return;

    const glm::ivec3 local = Coords::LocalOf({ x, y, z });
    const Block previous = chunk->GetBlock(local.x, local.y, local.z);
    if (previous == block) return;

    chunk->SetBlock(local.x, local.y, local.z, block);
    chunk->MarkUnsaved();
    if (previous.type == BlockType::PUMP && block.type != BlockType::PUMP) water.RemovePump({ x, y, z });
    if (block.type == BlockType::PUMP) water.AddPump({ x, y, z });

    // Water around the change may need to flow (or settle).
    water.Activate({ x, y, z });

    // Water only changing volume (most of what flowing water does) can't affect light,
    // and only changes what clients draw if its drawn surface height changes. Those
    // changes aren't sent: clients get the new volume with the next visible change.
    const bool waterVolumeOnly = previous.DiffersOnlyInWaterVolume(block);
    if (waterVolumeOnly) {
        // Water resting on water (a lake's top layer) is drawn down to a 32nd.
        Block below;
        const bool onWater = GetBlock(x, y - 1, z, below) && below.HasWater();
        if (previous.WaterSurface32nds(onWater) == block.WaterSurface32nds(onWater)) return;
    }

    {
        std::lock_guard<std::mutex> lock(logMutex);
        changedBlocks.emplace_back(x, y, z);
    }

    // Plants need the block under them: any change there uproots them.
    Block above;
    if (GetBlock(x, y + 1, z, above) && above.IsPlant()) SetBlock(x, y + 1, z, Block());

    // Light depends on the new block, so let the worker update it.
    if (!waterVolumeOnly) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            lightQueue.emplace_back(x, y, z);
        }
        queueCv.notify_one();
    }
}

void ServerWorld::TakeChanges(std::vector<glm::ivec3>& blocks, std::vector<glm::ivec3>& lightChunks) {
    blocks.clear();
    lightChunks.clear();
    std::lock_guard<std::mutex> lock(logMutex);
    blocks.swap(changedBlocks);
    lightChunks.swap(changedLight);
}

void ServerWorld::SetWaterSimulationDistance(int chunks) {
    waterDistance = std::clamp(chunks, 1, std::min(RENDER_DISTANCE, VERTICAL_RENDER_DISTANCE) - 1);
}

void ServerWorld::Update(const std::vector<glm::ivec3>& centers) {
    if (centers == lastCenters) return;
    lastCenters = centers;

    auto inRange = [&](const glm::ivec3& coords, int slack) {
        for (const auto& center : centers) {
            const glm::ivec3 offset = coords - center;
            const glm::ivec3 d = glm::abs(offset);
            if (d.x <= RENDER_DISTANCE + slack && d.z <= RENDER_DISTANCE + slack && offset.y >= -VERTICAL_RENDER_DISTANCE - slack &&
                offset.y < VERTICAL_RENDER_DISTANCE + slack)
                return true;
        }
        return false;
    };

    {
        // The edit mutex makes sure no edit lands in a chunk after it was saved here
        // and before it's marked removed (SetBlock checks IsRemoved under it).
        std::unique_ptr<Chunk::Blocks> saveBuffer;
        std::lock_guard<std::recursive_mutex> edit(editMutex);
        // Parked water further out than this sits in chunks that are being unloaded.
        water.SetSimulationArea(centers, waterDistance, std::max(RENDER_DISTANCE, VERTICAL_RENDER_DISTANCE) + 1);
        std::unique_lock<std::shared_mutex> lock(chunksMutex);
        for (auto it = chunks.begin(); it != chunks.end();) {
            // Unload chunks that have left every player's range (with one chunk of
            // hysteresis so walking back and forth across a border doesn't thrash).
            // Placeholders that were never generated have nothing to keep.
            const bool farAway = !inRange(it->first, 1);
            const bool stalePlaceholder = it->second->GetState() == Chunk::State::Pending && !inRange(it->first, 0);
            if (farAway || stalePlaceholder) {
                if (storage && it->second->IsLoaded() && it->second->TakeUnsaved()) {
                    if (!saveBuffer) saveBuffer = std::make_unique<Chunk::Blocks>();
                    it->second->CopyBlocks(*saveBuffer);
                    storage->StoreChunk(it->first, *saveBuffer);
                }
                it->second->MarkRemoved();
                it = chunks.erase(it);
            }
            else {
                ++it;
            }
        }

        // Create placeholders for any missing chunks in range.
        for (const auto& center : centers)
            for (const auto& offset : loadOffsets) {
                const glm::ivec3 coords = center + offset;
                if (chunks.find(coords) == chunks.end()) chunks.emplace(coords, std::make_shared<Chunk>(coords));
            }
    }

    // Re-prioritise the generate queue nearest-first around the players, taking turns
    // between them ring by ring. Chunks already being generated are skipped by
    // TryBeginGenerate.
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        generateQueue.clear();
        std::unordered_set<glm::ivec3, Coords::IVec3Hash> queued;
        for (const auto& offset : loadOffsets)
            for (const auto& center : centers) {
                const auto& chunk = chunks.at(center + offset);
                if (chunk->GetState() == Chunk::State::Pending && queued.insert(center + offset).second) generateQueue.push_back(chunk);
            }
    }
    queueCv.notify_one();
}

const TerrainGenerator::ColumnHeights& ServerWorld::GetColumnHeights(int chunkX, int chunkZ) {
    // Enough for every column in range plus the hysteresis band, with room to move.
    const size_t maxEntries = 4 * static_cast<size_t>(2 * RENDER_DISTANCE + 5) * (2 * RENDER_DISTANCE + 5);
    if (columnHeights.size() > maxEntries) columnHeights.clear();

    auto& entry = columnHeights[glm::ivec2(chunkX, chunkZ)];
    if (!entry) {
        entry = std::make_unique<TerrainGenerator::ColumnHeights>();
        terrain.ComputeColumnHeights(chunkX, chunkZ, *entry);
    }
    return *entry;
}

void ServerWorld::PropagateLight(const std::vector<ChunkPtr>& loadedChunks, const std::vector<glm::ivec3>& changedBlockPositions) {
    // Sky light coming down into each new chunk's columns, from the generator, for
    // columns whose chunk above hasn't loaded yet.
    constexpr int S = Chunk::CHUNK_SIZE;
    std::vector<std::array<uint8_t, S * S>> skyFromAbove(loadedChunks.size());
    for (size_t i = 0; i < loadedChunks.size(); i++) {
        const glm::ivec3 c = loadedChunks[i]->GetCoords();
        const auto& heights = GetColumnHeights(c.x, c.z);
        const int chunkTop = c.y * S + S - 1;
        // Saved chunks above may have been built on: no sky light where one of them
        // has something in the way (too dark is fine, it's corrected once they load).
        WorldStorage::ColumnMask blocked{};
        if (storage) storage->SkyBlockedAbove(c, blocked);
        for (int x = 0; x < S; x++)
            for (int z = 0; z < S; z++)
                skyFromAbove[i][x * S + z] = WorldStorage::IsColumnSet(blocked, x, z)
                                                 ? 0
                                                 : TerrainGenerator::ColumnFrom(heights, x, z).SkyLightAbove(chunkTop);
    }

    LightPropagator::ChunkSet dirty;
    std::vector<glm::ivec3> published;
    {
        // Held shared for the whole pass: the propagator keeps raw chunk pointers,
        // and this stops the server thread unloading chunks underneath it.
        std::shared_lock<std::shared_mutex> lock(chunksMutex);
        for (size_t i = 0; i < loadedChunks.size(); i++)
            if (!loadedChunks[i]->IsRemoved()) light.OnChunkLoaded(*loadedChunks[i], &skyFromAbove[i]);
        for (const auto& pos : changedBlockPositions)
            light.OnBlockChanged(pos);
        light.Propagate(dirty);

        // New chunks are published even if propagation didn't touch them (all dark),
        // so they can be sent. `dirty` also holds neighbours whose meshes sample a
        // changed cell; PublishLight skips those whose own light is unchanged.
        for (const auto& chunk : loadedChunks) dirty.insert(chunk->GetCoords());
        for (const auto& coords : dirty) {
            auto chunk = FindChunkUnlocked(coords);
            if (chunk && chunk->IsLoaded() && !chunk->IsRemoved() && chunk->PublishLight()) published.push_back(coords);
        }
    }

    std::lock_guard<std::mutex> lock(logMutex);
    changedLight.insert(changedLight.end(), published.begin(), published.end());
}

void ServerWorld::WorkerLoop() {
    std::vector<ChunkPtr> batch, generated;
    auto generatedBlocks = std::make_unique<TerrainGenerator::Blocks>(); // reused generation buffer
    std::vector<glm::ivec3> edits, savedWater, savedPumps;
    batch.reserve(kGenerateBatchSize);

    while (true) {
        batch.clear();
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [this] { return stopping || !generateQueue.empty() || !lightQueue.empty(); });
            if (stopping) return;

            while (!generateQueue.empty() && batch.size() < kGenerateBatchSize) {
                batch.push_back(std::move(generateQueue.front()));
                generateQueue.pop_front();
            }
            edits.swap(lightQueue);
            lightQueue.clear();
        }

        generated.clear();
        savedWater.clear();
        savedPumps.clear();
        for (auto& chunk : batch) {
            if (chunk->IsRemoved() || !chunk->TryBeginGenerate()) continue;

            const glm::ivec3 c = chunk->GetCoords();
            constexpr int S = Chunk::CHUNK_SIZE;
            const bool saved = storage && storage->LoadChunk(c, *generatedBlocks);
            if (!saved) terrain.GenerateChunk(c, GetColumnHeights(c.x, c.z), *generatedBlocks);
            // Pumps (saved, or in a dungeon's pump room) start working; water in a saved
            // chunk may have been moving when it was saved. Generated water is at rest.
            for (int i = 0; i < Chunk::VOLUME; i++) {
                const Block& b = (*generatedBlocks)[i];
                const bool pump = b.type == BlockType::PUMP;
                if (!pump && !(saved && b.HasWater())) continue;
                const glm::ivec3 pos = c * S + glm::ivec3(i % S, (i / S) % S, i / (S * S));
                (pump ? savedPumps : savedWater).push_back(pos);
            }
            chunk->SetGenerated(*generatedBlocks);
            generated.push_back(chunk);
        }

        if (!generated.empty() || !edits.empty()) PropagateLight(generated, edits);

        // Let it carry on flowing (settled water just stays put after one look).
        if (!savedWater.empty() || !savedPumps.empty()) {
            std::lock_guard<std::recursive_mutex> edit(editMutex);
            for (const auto& pos : savedWater) water.Activate(pos);
            for (const auto& pos : savedPumps) water.AddPump(pos);
        }
    }
}

size_t ServerWorld::SaveAll() {
    if (!storage) return 0;
    std::vector<std::pair<glm::ivec3, std::unique_ptr<Chunk::Blocks>>> snapshot;
    {
        std::lock_guard<std::recursive_mutex> edit(editMutex);
        for (const auto& [coords, chunk] : chunks) {
            if (!chunk->IsLoaded() || chunk->IsRemoved() || !chunk->TakeUnsaved()) continue;
            auto blocks = std::make_unique<Chunk::Blocks>();
            chunk->CopyBlocks(*blocks);
            snapshot.emplace_back(coords, std::move(blocks));
        }
    }
    // Encoding happens outside the edit mutex, so water keeps flowing meanwhile.
    for (const auto& [coords, blocks] : snapshot) storage->StoreChunk(coords, *blocks);
    return snapshot.size();
}
