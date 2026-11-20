#include "world/World.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <glm/gtc/matrix_transform.hpp>

#include "render/Shader.h"

namespace {

int FloorDiv(int a, int b) {
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

int FloorMod(int a, int b) {
    return ((a % b) + b) % b;
}

// Number of generate jobs the worker takes per batch before servicing mesh
// rebuilds. Batching lets neighbouring chunks load before their meshes are built,
// which avoids rebuilding the same chunk several times.
constexpr size_t kGenerateBatchSize = 16;

} // namespace

World::World(const TerrainGenerator& terrain) : terrain(terrain) {
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
    return { FloorDiv(x, Chunk::CHUNK_SIZE), FloorDiv(y, Chunk::CHUNK_SIZE), FloorDiv(z, Chunk::CHUNK_SIZE) };
}

glm::ivec3 World::WorldToLocalCoordinates(int x, int y, int z) {
    return { FloorMod(x, Chunk::CHUNK_SIZE), FloorMod(y, Chunk::CHUNK_SIZE), FloorMod(z, Chunk::CHUNK_SIZE) };
}

World::ChunkPtr World::FindChunk(const glm::ivec3& chunkCoords) const {
    auto it = chunks.find(chunkCoords);
    return it != chunks.end() ? it->second : nullptr;
}

bool World::GetBlock(int x, int y, int z, Block& block) const {
    auto chunk = FindChunk(WorldToChunkCoordinates(x, y, z));
    if (!chunk || !chunk->IsLoaded()) return false;

    auto local = WorldToLocalCoordinates(x, y, z);
    block = chunk->GetBlock(local.x, local.y, local.z);
    return true;
}

void World::SetBlock(int x, int y, int z, const Block& block) {
    const glm::ivec3 chunkCoords = WorldToChunkCoordinates(x, y, z);
    auto chunk = FindChunk(chunkCoords);
    // Edits to chunks that haven't generated yet would be overwritten by generation.
    if (!chunk || !chunk->IsLoaded()) return;

    const glm::ivec3 local = WorldToLocalCoordinates(x, y, z);
    if (chunk->GetBlock(local.x, local.y, local.z) == block) return;

    chunk->SetBlock(local.x, local.y, local.z, block);
    RequestRebuild(chunk);

    // Blocks on a chunk border also affect the neighbouring chunk's mesh.
    const int last = Chunk::CHUNK_SIZE - 1;
    for (int axis = 0; axis < 3; axis++) {
        int offset = local[axis] == 0 ? -1 : local[axis] == last ? 1 : 0;
        if (offset == 0) continue;

        glm::ivec3 neighbourCoords = chunkCoords;
        neighbourCoords[axis] += offset;
        auto neighbour = FindChunk(neighbourCoords);
        if (neighbour && neighbour->IsLoaded()) RequestRebuild(neighbour);
    }
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
    {
        std::unique_lock<std::shared_mutex> lock(chunksMutex);

        // Unload chunks that have left the render distance (with one chunk of
        // hysteresis so walking back and forth across a border doesn't thrash).
        for (auto it = chunks.begin(); it != chunks.end();) {
            const glm::ivec3 d = glm::abs(it->first - center);
            if (d.x > RENDER_DISTANCE + 1 || d.z > RENDER_DISTANCE + 1 || d.y > VERTICAL_RENDER_DISTANCE + 1) {
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
        for (const auto& o : kFaceOffsets) {
            auto neighbour = FindChunk(chunkCoords + glm::ivec3(o[0], o[1], o[2]));
            if (neighbour && neighbour->IsLoaded()) neighbours.push_back(std::move(neighbour));
        }
    }
    for (const auto& neighbour : neighbours) RequestRebuild(neighbour);
}

void World::BuildChunkMesh(Chunk& chunk) {
    Chunk::Neighbours neighbours;
    {
        std::shared_lock<std::shared_mutex> lock(chunksMutex);
        const glm::ivec3 coords = chunk.GetCoords();
        for (int face = 0; face < kFaceCount; face++) {
            const auto& o = kFaceOffsets[face];
            neighbours[face] = FindChunk(coords + glm::ivec3(o[0], o[1], o[2]));
        }
    }
    chunk.BuildMesh(neighbours);
}

void World::WorkerLoop() {
    std::vector<ChunkPtr> batch;
    batch.reserve(kGenerateBatchSize);

    while (true) {
        batch.clear();
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [this] { return stopping || !generateQueue.empty() || !rebuildQueue.empty(); });
            if (stopping) return;

            while (!generateQueue.empty() && batch.size() < kGenerateBatchSize) {
                batch.push_back(std::move(generateQueue.front()));
                generateQueue.pop_front();
            }
        }

        for (auto& chunk : batch) {
            if (chunk->IsRemoved() || !chunk->TryBeginGenerate()) continue;

            chunk->Generate(terrain);
            RequestRebuild(chunk);
            RequestNeighbourRebuilds(chunk->GetCoords());
        }

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
