#include "world/World.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <glm/gtc/matrix_transform.hpp>

#include "render/Shader.h"

namespace {

// Number of generate jobs the worker takes per batch before servicing mesh
// rebuilds. Batching lets neighbouring chunks load before their meshes are built,
// which avoids rebuilding the same chunk several times.
constexpr size_t kGenerateBatchSize = 32;

} // namespace

World::World(const TerrainGenerator& terrain)
    : terrain(terrain),
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

    worker = std::thread(&World::WorkerLoop, this);
}

World::~World() {
    water.Stop(); // uses the chunks, so it goes first
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopping = true;
    }
    queueCv.notify_all();
    if (worker.joinable()) worker.join();

    for (auto& [coords, chunk] : chunks) {
        chunk->MarkRemoved();
        chunk->ReleaseGpu();
    }
}

glm::ivec3 World::WorldToChunkCoordinates(const glm::vec3& position) {
    return WorldToChunkCoordinates(static_cast<int>(std::floor(position.x)),
                                   static_cast<int>(std::floor(position.y)),
                                   static_cast<int>(std::floor(position.z)));
}

glm::ivec3 World::WorldToChunkCoordinates(int x, int y, int z) {
    return Coords::ChunkOf({ x, y, z });
}

glm::ivec3 World::WorldToLocalCoordinates(int x, int y, int z) {
    return Coords::LocalOf({ x, y, z });
}

World::ChunkPtr World::FindChunk(const glm::ivec3& chunkCoords) const {
    auto it = chunks.find(chunkCoords);
    return it != chunks.end() ? it->second : nullptr;
}

World::ChunkPtr World::FindChunkShared(const glm::ivec3& chunkCoords) const {
    std::shared_lock<std::shared_mutex> lock(chunksMutex);
    return FindChunk(chunkCoords);
}

bool World::GetBlock(int x, int y, int z, Block& block) const {
    auto chunk = FindChunkShared(WorldToChunkCoordinates(x, y, z));
    if (!chunk || !chunk->IsLoaded()) return false;

    auto local = WorldToLocalCoordinates(x, y, z);
    block = chunk->GetBlock(local.x, local.y, local.z);
    return true;
}

void World::SetBlock(int x, int y, int z, const Block& block) {
    struct PendingEdit {
        std::atomic<int>& count;
        explicit PendingEdit(std::atomic<int>& c) : count(c) { count.fetch_add(1, std::memory_order_acq_rel); }
        ~PendingEdit() { count.fetch_sub(1, std::memory_order_acq_rel); }
    } pending(pendingEdits);
    std::lock_guard<std::recursive_mutex> edit(editMutex);
    const glm::ivec3 chunkCoords = WorldToChunkCoordinates(x, y, z);
    auto chunk = FindChunkShared(chunkCoords);
    // Edits to chunks that haven't generated yet would be overwritten by generation,
    // and unloaded ones are gone.
    if (!chunk || !chunk->IsLoaded() || chunk->IsRemoved()) return;

    const glm::ivec3 local = WorldToLocalCoordinates(x, y, z);
    const Block previous = chunk->GetBlock(local.x, local.y, local.z);
    if (previous == block) return;

    chunk->SetBlock(local.x, local.y, local.z, block);

    // Water around the change may need to flow (or settle).
    water.Activate({ x, y, z });

    // Water only changing volume (most of what flowing water does) can't affect light,
    // and only needs new meshes if its drawn surface height changes.
    const bool waterVolumeOnly = previous.DiffersOnlyInWaterVolume(block);
    if (waterVolumeOnly && previous.WaterSurfaceEighths() == block.WaterSurfaceEighths()) return;

    // Plants need the block under them: any change there uproots them.
    Block above;
    if (GetBlock(x, y + 1, z, above) && above.IsPlant()) SetBlock(x, y + 1, z, Block());

    // Light depends on the new block, so let the worker update it; it requests the
    // mesh rebuilds once light is settled. Light-neutral edits (no light at or
    // around the block) still need rebuilding, so request those directly too.
    if (!waterVolumeOnly) {
        std::lock_guard<std::mutex> lock(queueMutex);
        lightQueue.emplace_back(x, y, z);
    }
    queueCv.notify_one();

    Coords::ForEachChunkTouching({ x, y, z }, [&](const glm::ivec3& coords) {
        auto touched = coords == chunkCoords ? chunk : FindChunkShared(coords);
        if (touched && touched->IsLoaded()) RequestRebuild(touched);
    });
}

bool World::IsUnderwater(const glm::vec3& point) const {
    const glm::ivec3 cell = glm::ivec3(glm::floor(point));
    Block block;
    if (!GetBlock(cell.x, cell.y, cell.z, block) || !block.HasWater()) return false;
    Block above;
    if (GetBlock(cell.x, cell.y + 1, cell.z, above) && above.HasWater()) return true; // full under more water
    return point.y - cell.y < block.WaterLevel() / static_cast<float>(Block::kMaxWaterVolume);
}

void World::RebuildAllChunks() {
    for (auto& [coords, chunk] : chunks) {
        if (chunk->IsLoaded()) RequestRebuild(chunk);
    }
}

void World::Update(const glm::vec3& cameraPosition) {
    const glm::ivec3 center = WorldToChunkCoordinates(cameraPosition);
    if (hasCenter && center == lastCenter) return;

    lastCenter = center;
    hasCenter = true;
    RefreshChunksAround(center);
}

void World::RefreshChunksAround(const glm::ivec3& center) {
    // Loaded neighbours of dropped placeholders. Their meshes may have been
    // deferred waiting for those placeholders (see BuildChunkMesh).
    std::vector<glm::ivec3> unblocked;
    {
        std::unique_lock<std::shared_mutex> lock(chunksMutex);

        for (auto it = chunks.begin(); it != chunks.end();) {
            const glm::ivec3 offset = it->first - center;
            const glm::ivec3 d = glm::abs(offset);
            // Unload chunks that have left the render distance (with one chunk of
            // hysteresis so walking back and forth across a border doesn't thrash).
            const bool farAway = d.x > RENDER_DISTANCE + 1 || d.z > RENDER_DISTANCE + 1 || d.y > VERTICAL_RENDER_DISTANCE + 1;
            // Placeholders that were never generated have nothing to keep, and must not
            // linger outside the load range: they would never be queued, so neighbours
            // waiting on them would never mesh.
            const bool inLoadRange = d.x <= RENDER_DISTANCE && d.z <= RENDER_DISTANCE &&
                                     offset.y >= -VERTICAL_RENDER_DISTANCE && offset.y < VERTICAL_RENDER_DISTANCE;
            const bool stalePlaceholder = !inLoadRange && it->second->GetState() == Chunk::State::Pending;

            if (farAway || stalePlaceholder) {
                if (stalePlaceholder) unblocked.push_back(it->first);
                it->second->MarkRemoved();
                it->second->ReleaseGpu();
                it = chunks.erase(it);
            }
            else {
                ++it;
            }
        }

        // Create placeholders for any missing chunks in range.
        for (const auto& offset : loadOffsets) {
            const glm::ivec3 coords = center + offset;
            if (chunks.find(coords) == chunks.end()) {
                chunks.emplace(coords, std::make_shared<Chunk>(coords));
            }
        }
    }

    for (const auto& coords : unblocked) {
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dz = -1; dz <= 1; dz++) {
                    auto neighbour = FindChunk(coords + glm::ivec3(dx, dy, dz));
                    if (neighbour && neighbour->IsLoaded()) RequestRebuild(neighbour);
                }
    }

    // Re-prioritise the generate queue nearest-first around the new center. Chunks
    // that are already being generated are skipped by TryBeginGenerate.
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        generateQueue.clear();
        for (const auto& offset : loadOffsets) {
            const auto& chunk = chunks.at(center + offset);
            if (chunk->GetState() == Chunk::State::Pending) {
                generateQueue.push_back(chunk);
            }
        }
    }
    queueCv.notify_one();
}

void World::RequestRebuild(const ChunkPtr& chunk) {
    if (!chunk->MarkDirty()) return; // already queued

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        rebuildQueue.push_back(chunk);
    }
    queueCv.notify_one();
}

void World::RequestNeighbourRebuilds(const glm::ivec3& chunkCoords) {
    std::vector<ChunkPtr> neighbours;
    {
        std::shared_lock<std::shared_mutex> lock(chunksMutex);
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dz = -1; dz <= 1; dz++) {
                    if (dx == 0 && dy == 0 && dz == 0) continue;
                    auto neighbour = FindChunk(chunkCoords + glm::ivec3(dx, dy, dz));
                    if (neighbour && neighbour->IsLoaded()) neighbours.push_back(std::move(neighbour));
                }
    }
    for (const auto& neighbour : neighbours) RequestRebuild(neighbour);
}

const TerrainGenerator::ColumnHeights& World::GetColumnHeights(int chunkX, int chunkZ) {
    // Enough for every column in range plus the hysteresis band, with room to move.
    const size_t maxEntries = 2 * static_cast<size_t>(2 * RENDER_DISTANCE + 5) * (2 * RENDER_DISTANCE + 5);
    if (columnHeights.size() > maxEntries) columnHeights.clear();

    auto& entry = columnHeights[glm::ivec2(chunkX, chunkZ)];
    if (!entry) {
        entry = std::make_unique<TerrainGenerator::ColumnHeights>();
        terrain.ComputeColumnHeights(chunkX, chunkZ, *entry);
    }
    return *entry;
}

bool World::BuildChunkMesh(Chunk& chunk) {
    Chunk::Neighbours neighbours;
    {
        std::shared_lock<std::shared_mutex> lock(chunksMutex);
        const glm::ivec3 coords = chunk.GetCoords();
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dz = -1; dz <= 1; dz++) {
                    if (dx == 0 && dy == 0 && dz == 0) continue;
                    auto neighbour = FindChunk(coords + glm::ivec3(dx, dy, dz));
                    // A neighbour that exists but hasn't generated yet will change this
                    // mesh (culling, AO and light at the border) once it does. Skip the
                    // build: when it finishes it requests a rebuild of its loaded
                    // neighbours, including this one. Without this, every chunk would be
                    // meshed again as each of its 26 neighbours arrives.
                    if (neighbour && !neighbour->IsLoaded() && !neighbour->IsRemoved()) return false;
                    neighbours[Chunk::NeighbourIndex(dx, dy, dz)] = std::move(neighbour);
                }
    }
    chunk.BuildMesh(neighbours);
    return true;
}

void World::PropagateLight(const std::vector<ChunkPtr>& loadedChunks, const std::vector<glm::ivec3>& changedBlocks) {
    // Sky light coming down into each new chunk's columns, from the generator, for
    // columns whose chunk above hasn't loaded yet.
    constexpr int S = Chunk::CHUNK_SIZE;
    std::vector<std::array<uint8_t, S * S>> skyFromAbove(loadedChunks.size());
    for (size_t i = 0; i < loadedChunks.size(); i++) {
        const glm::ivec3 c = loadedChunks[i]->GetCoords();
        const auto& heights = GetColumnHeights(c.x, c.z);
        const int chunkTop = c.y * S + S - 1;
        for (int x = 0; x < S; x++)
            for (int z = 0; z < S; z++)
                skyFromAbove[i][x * S + z] = TerrainGenerator::ColumnFrom(heights, x, z).SkyLightAbove(chunkTop);
    }

    LightPropagator::ChunkSet dirty;
    {
        // Held shared for the whole pass: the propagator keeps raw chunk pointers,
        // and this stops the main thread unloading chunks underneath it.
        std::shared_lock<std::shared_mutex> lock(chunksMutex);
        for (size_t i = 0; i < loadedChunks.size(); i++)
            if (!loadedChunks[i]->IsRemoved()) light.OnChunkLoaded(*loadedChunks[i], &skyFromAbove[i]);
        for (const auto& pos : changedBlocks)
            light.OnBlockChanged(pos);
        light.Propagate(dirty);
    }

    std::vector<ChunkPtr> toRebuild;
    {
        std::shared_lock<std::shared_mutex> lock(chunksMutex);
        for (const auto& coords : dirty) {
            auto chunk = FindChunk(coords);
            if (chunk && chunk->IsLoaded()) toRebuild.push_back(std::move(chunk));
        }
    }
    for (const auto& chunk : toRebuild) RequestRebuild(chunk);
}

void World::WorkerLoop() {
    std::vector<ChunkPtr> batch, generated;
    auto generatedBlocks = std::make_unique<TerrainGenerator::Blocks>(); // reused generation buffer
    std::vector<glm::ivec3> changedBlocks;
    batch.reserve(kGenerateBatchSize);

    while (true) {
        batch.clear();
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [this] {
                return stopping || !generateQueue.empty() || !rebuildQueue.empty() || !lightQueue.empty();
            });
            if (stopping) return;

            while (!generateQueue.empty() && batch.size() < kGenerateBatchSize) {
                batch.push_back(std::move(generateQueue.front()));
                generateQueue.pop_front();
            }
            changedBlocks.swap(lightQueue);
            lightQueue.clear();
        }

        generated.clear();
        for (auto& chunk : batch) {
            if (chunk->IsRemoved() || !chunk->TryBeginGenerate()) continue;

            const glm::ivec3 c = chunk->GetCoords();
            terrain.GenerateChunk(c, GetColumnHeights(c.x, c.z), *generatedBlocks);
            chunk->SetGenerated(*generatedBlocks);
            generated.push_back(chunk);
            RequestRebuild(chunk);
            RequestNeighbourRebuilds(chunk->GetCoords());
        }

        // Settle light before meshing so new meshes see it.
        if (!generated.empty() || !changedBlocks.empty()) PropagateLight(generated, changedBlocks);

        // Drain mesh rebuilds. These include player edits, so they take priority
        // over generating more terrain.
        while (true) {
            ChunkPtr chunk;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (stopping || rebuildQueue.empty()) break;
                chunk = std::move(rebuildQueue.front());
                rebuildQueue.pop_front();
            }

            // Clear before building so an edit made during the build re-queues it.
            chunk->ClearDirty();
            if (chunk->IsRemoved() || !chunk->IsLoaded()) continue;

            BuildChunkMesh(*chunk);
        }
    }
}

void World::Render(Shader& shader, const glm::mat4& view, const glm::mat4& projection, float time) {
    shader.Use();
    shader.SetUniform("time", time);
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);

    for (auto& [coords, chunk] : chunks) {
        if (!chunk->IsLoaded()) continue;

        chunk->UploadPendingMesh();
        if (!chunk->HasGeometry()) continue;

        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(coords * Chunk::CHUNK_SIZE));
        shader.SetUniform("model", model);
        chunk->Draw();
    }
}

void World::RenderWater(Shader& shader, const glm::mat4& view, const glm::mat4& projection, float time, const glm::vec3& cameraPosition) {
    // Back to front so overlapping translucent surfaces blend correctly (per chunk;
    // within a chunk water is mostly a single surface).
    std::vector<std::pair<float, Chunk*>> order;
    for (auto& [coords, chunk] : chunks) {
        if (!chunk->IsLoaded() || !chunk->HasWater()) continue;
        const glm::vec3 centre = glm::vec3(coords * Chunk::CHUNK_SIZE) + glm::vec3(Chunk::CHUNK_SIZE * 0.5f);
        const glm::vec3 d = centre - cameraPosition;
        order.emplace_back(glm::dot(d, d), chunk.get());
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    shader.Use();
    shader.SetUniform("time", time);
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    for (const auto& [distance, chunk] : order) {
        shader.SetUniform("model", glm::translate(glm::mat4(1.0f), glm::vec3(chunk->GetCoords() * Chunk::CHUNK_SIZE)));
        chunk->DrawWater();
    }
}
