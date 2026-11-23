#include "server/ServerWorld.h"

#include "server/WorldStorage.h"
#include "world/Power.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {

// Number of generate jobs the worker takes per batch before propagating light.
// Batching lets neighbouring chunks load together, so light settles across them in
// one pass.
constexpr size_t kGenerateBatchSize = 32;

} // namespace

ServerWorld::ServerWorld(const TerrainGenerator& terrain, WorldStorage* storage, int distance)
    : terrain(terrain),
      storage(storage),
      light([this](const glm::ivec3& coords) -> Chunk* {
          // Called with chunksMutex held shared (see PropagateLight).
          auto it = chunks.find(coords);
          return it != chunks.end() && it->second->IsLoaded() && !it->second->IsRemoved() ? it->second.get() : nullptr;
      }) {
    renderDistance = std::clamp(distance, 2, RENDER_DISTANCE);
    verticalRenderDistance = std::min(VERTICAL_RENDER_DISTANCE, std::max(renderDistance, 4));
    const int r = renderDistance;
    const int v = verticalRenderDistance;
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
    // (Springs and drains run with the pumps.)
    const bool wasPump = previous.type == BlockType::PUMP || previous.type == BlockType::SPRING || IsDrainType(previous.type) || previous.type == BlockType::FLUID_OUTLET;
    const bool isPump = block.type == BlockType::PUMP || block.type == BlockType::SPRING || IsDrainType(block.type) || block.type == BlockType::FLUID_OUTLET;
    if (wasPump && previous.type != block.type) water.RemovePump({ x, y, z });
    if (isPump) water.AddPump({ x, y, z });
    if (Power::IsMachine(previous.type) && previous.type != block.type) machines.erase({ x, y, z });
    if (Power::IsMachine(block.type)) machines.insert({ x, y, z });
    // Lava: track it, and wake it (and lava beside it) when anything here changes, so
    // it runs on, drains away, or boils water that came up to it.
    if (previous.IsLava() && !block.IsLava()) lavaCells.erase({ x, y, z });
    if (block.IsLava()) lavaCells.insert({ x, y, z }), lavaWake.insert({ x, y, z });
    if (!lavaCells.empty())
        for (int f = 0; f < kFaceCount; f++) {
            const glm::ivec3 n(x + kFaceOffsets[f][0], y + kFaceOffsets[f][1], z + kFaceOffsets[f][2]);
            if (lavaCells.count(n)) lavaWake.insert(n);
        }
    const bool wasWheel = previous.type == BlockType::WATER_WHEEL, isWheel = block.type == BlockType::WATER_WHEEL;
    if (wasWheel && (!isWheel || previous.axis != block.axis)) WatchWheel({ x, y, z }, previous, false);
    if (isWheel && (!wasWheel || previous.axis != block.axis)) WatchWheel({ x, y, z }, block, true);
    // Water moving through a wheel's paddles turns it.
    if (previous.water != block.water && !wheelWatch.empty()) NoteWaterMoved({ x, y, z }, std::abs(int(block.water) - int(previous.water)));

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

    // Plants need the block under them: any change there uproots them. An open door
    // counts as one, but its upper half stays on as long as there's a door half under
    // it (opening or closing a door changes the halves one at a time).
    Block above;
    const bool doorBelow = block.type == BlockType::DOOR || block.type == BlockType::DOOR_OPEN;
    if (GetBlock(x, y + 1, z, above) && above.IsPlant() && !IsLeverType(above.type) && !(above.type == BlockType::DOOR_OPEN && doorBelow)) {
        SetBlock(x, y + 1, z, Block());
        // (Dropped as an item, by the server; not a door's upper half: its worth is the door's.)
        if (above.type != BlockType::DOOR_OPEN) {
            std::lock_guard<std::mutex> lock(logMutex);
            knockedOff.emplace_back(glm::ivec3(x, y + 1, z), above);
        }
    }
    // Levers fixed to this block fall off if it no longer holds them.
    if (Block::HoldsLever(previous) && !Block::HoldsLever(block))
        for (int f = 0; f < kFaceCount; f++) {
            const glm::ivec3 n(x + kFaceOffsets[f][0], y + kFaceOffsets[f][1], z + kFaceOffsets[f][2]);
            Block lever;
            if (!GetBlock(n.x, n.y, n.z, lever) || !IsLeverType(lever.type) || n - lever.LeverFacing() != glm::ivec3(x, y, z)) continue;
            SetBlock(n.x, n.y, n.z, Block());
            std::lock_guard<std::mutex> lock(logMutex);
            knockedOff.emplace_back(n, lever); // (dropped as an item: Server)
        }

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
    waterDistance = std::clamp(chunks, 1, std::min(renderDistance, verticalRenderDistance) - 1);
}

void ServerWorld::Update(const std::vector<glm::ivec3>& centers) {
    if (centers == lastCenters) return;
    lastCenters = centers;
    // Chunks dropped: freed on a thread of their own once the locks are let go (a player
    // teleporting far drops thousands at once, and freeing them all takes a while).
    std::vector<std::shared_ptr<Chunk>> dropped;

    auto inRange = [&](const glm::ivec3& coords, int slack) {
        for (const auto& center : centers) {
            const glm::ivec3 offset = coords - center;
            const glm::ivec3 d = glm::abs(offset);
            if (d.x <= renderDistance + slack && d.z <= renderDistance + slack && offset.y >= -verticalRenderDistance - slack &&
                offset.y < verticalRenderDistance + slack)
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
        water.SetSimulationArea(centers, waterDistance, std::max(renderDistance, verticalRenderDistance) + 1);
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
                dropped.push_back(std::move(it->second));
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
        std::unordered_set<glm::ivec3, Coords::IVec3Hash> queued; // (players near each other share chunks)
        for (const auto& offset : loadOffsets)
            for (const auto& center : centers) {
                const auto& chunk = chunks.at(center + offset);
                if (chunk->GetState() == Chunk::State::Pending && (centers.size() == 1 || queued.insert(center + offset).second)) generateQueue.push_back(chunk);
            }
    }
    queueCv.notify_one();
    if (!dropped.empty()) std::thread([gone = std::move(dropped)]() mutable { gone.clear(); }).detach();
}

const TerrainGenerator::ColumnHeights& ServerWorld::GetColumnHeights(int chunkX, int chunkZ) {
    // Enough for every column in range plus the hysteresis band, with room to move.
    const size_t maxEntries = 4 * static_cast<size_t>(2 * renderDistance + 5) * (2 * renderDistance + 5);
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
                                                 : TerrainGenerator::ColumnFrom(heights, x, z).SkyLightAbove(chunkTop, terrain.WaterLevelAt(c.x * S + x, c.z * S + z));
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
    std::vector<glm::ivec3> edits, savedWater, savedPumps, savedMachines, savedLava, restingLava;
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
        savedLava.clear();
        restingLava.clear();
        savedMachines.clear();
        for (auto& chunk : batch) {
            if (chunk->IsRemoved() || !chunk->TryBeginGenerate()) continue;

            const glm::ivec3 c = chunk->GetCoords();
            constexpr int S = Chunk::CHUNK_SIZE;
            const bool saved = storage && storage->LoadChunk(c, *generatedBlocks);
            if (saved)
                for (Block& b : *generatedBlocks) b.type = Current(b.type); // (old water pipes are fluid pipes now)
            if (!saved) terrain.GenerateChunk(c, GetColumnHeights(c.x, c.z), *generatedBlocks);
            // Pumps (saved, or in a dungeon's pump room) start working; water in a saved
            // chunk may have been moving when it was saved. Generated water is at rest.
            for (int i = 0; i < Chunk::VOLUME; i++) {
                const Block& b = (*generatedBlocks)[i];
                const bool pump = b.type == BlockType::PUMP || b.type == BlockType::SPRING || IsDrainType(b.type) || b.type == BlockType::FLUID_OUTLET;
                const bool machine = Power::IsMachine(b.type);
                // Lava: known wherever it is; woken only if it was saved (it may have been
                // flowing then). Generated pools start at rest, and flow once something
                // beside them changes.
                if (b.IsLava()) (saved ? savedLava : restingLava).push_back(c * S + glm::ivec3(i % S, (i / S) % S, i / (S * S)));
                if (!pump && !machine && !(saved && b.HasWater())) continue;
                const glm::ivec3 pos = c * S + glm::ivec3(i % S, (i / S) % S, i / (S * S));
                (pump ? savedPumps : machine ? savedMachines : savedWater).push_back(pos);
            }
            chunk->SetGenerated(*generatedBlocks);
            generated.push_back(chunk);
        }

        if (!generated.empty() || !edits.empty()) PropagateLight(generated, edits);

        // Let it carry on flowing (settled water just stays put after one look).
        if (!savedWater.empty() || !savedPumps.empty() || !savedMachines.empty() || !savedLava.empty() || !restingLava.empty()) {
            std::lock_guard<std::recursive_mutex> edit(editMutex);
            for (const auto& pos : savedLava) lavaCells.insert(pos), lavaWake.insert(pos);
            for (const auto& pos : restingLava) lavaCells.insert(pos);
            for (const auto& pos : savedWater) water.Activate(pos);
            for (const auto& pos : savedPumps) {
                water.AddPump(pos);
                Block b;
                if (GetBlock(pos.x, pos.y, pos.z, b) && (b.type == BlockType::PUMP || Power::IsMachine(b.type))) machines.insert(pos); // (pumps run on air; drains and outlets trade with fluid pipes; springs neither)
            }
            for (const auto& pos : savedMachines) {
                Block b;
                if (!GetBlock(pos.x, pos.y, pos.z, b) || !Power::IsMachine(b.type)) continue;
                if (machines.insert(pos).second && b.type == BlockType::WATER_WHEEL) WatchWheel(pos, b, true);
            }
        }
    }
}

void ServerWorld::WatchWheel(const glm::ivec3& pos, const Block& wheel, bool watch) {
    // The paddle cells on each side: the column of three across the wheel each way.
    const glm::ivec3 across = Power::AcrossOf(wheel);
    for (int side = 0; side < 2; side++)
        for (int dy = -1; dy <= 1; dy++) {
            const glm::ivec3 cell = pos + across * (side == 0 ? 1 : -1) + glm::ivec3(0, dy, 0);
            auto& list = wheelWatch[cell];
            list.erase(std::remove(list.begin(), list.end(), std::make_pair(pos, side)), list.end());
            if (watch) list.emplace_back(pos, side);
            if (list.empty()) wheelWatch.erase(cell);
        }
    if (watch) wheelFlow.try_emplace(pos, glm::vec2(0.0f));
    else wheelFlow.erase(pos);
}

void ServerWorld::NoteWaterMoved(const glm::ivec3& pos, int units) {
    auto it = wheelWatch.find(pos);
    if (it == wheelWatch.end()) return;
    for (const auto& [wheel, side] : it->second) {
        auto f = wheelFlow.find(wheel);
        if (f != wheelFlow.end()) f->second[side] += static_cast<float>(units);
    }
}

void ServerWorld::TakeLavaWake(std::vector<glm::ivec3>& out) {
    std::lock_guard<std::recursive_mutex> edit(editMutex);
    out.assign(lavaWake.begin(), lavaWake.end());
    lavaWake.clear();
    // Lava in chunks that unloaded is forgotten (it comes back when they load): looked
    // through now and then, not every step (there may be a lot of it in the Deep).
    if (++lavaPrune >= 20) {
        lavaPrune = 0;
        for (auto it = lavaCells.begin(); it != lavaCells.end();) {
            Block b;
            if (!GetBlock(it->x, it->y, it->z, b) || !b.IsLava()) it = lavaCells.erase(it);
            else ++it;
        }
    }
}

void ServerWorld::TakePower(std::vector<glm::ivec3>& machinesOut, std::unordered_map<glm::ivec3, glm::vec2, Coords::IVec3Hash>& flow) {
    std::lock_guard<std::recursive_mutex> edit(editMutex);
    machinesOut.clear();
    flow.clear();
    // Machines in chunks that unloaded are dropped (they come back when it loads).
    for (auto it = machines.begin(); it != machines.end();) {
        Block b;
        if (!GetBlock(it->x, it->y, it->z, b) || !Power::IsMachine(b.type)) {
            if (wheelFlow.count(*it)) {
                Block wheel(BlockType::WATER_WHEEL);
                for (const uint8_t axis : { Block::kAlongX, Block::kAlongZ }) { wheel.axis = axis; WatchWheel(*it, wheel, false); }
            }
            it = machines.erase(it);
            continue;
        }
        machinesOut.push_back(*it);
        if (auto f = wheelFlow.find(*it); f != wheelFlow.end()) {
            flow[*it] = f->second;
            f->second = glm::vec2(0.0f);
        }
        ++it;
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
