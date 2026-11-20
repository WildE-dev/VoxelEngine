#include "client/ClientWorld.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>

#include "render/Shader.h"

ClientWorld::ClientWorld() {
    worker = std::thread(&ClientWorld::WorkerLoop, this);
}

ClientWorld::~ClientWorld() {
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

void ClientWorld::SetViewDistance(int horizontal, int vertical) {
    renderDistance = horizontal;
    verticalRenderDistance = vertical;
    loadOffsets.clear();
    for (int x = -horizontal; x <= horizontal; x++)
        for (int y = -vertical; y < vertical; y++)
            for (int z = -horizontal; z <= horizontal; z++) loadOffsets.emplace_back(x, y, z);
    hasCenter = false;
}

ClientWorld::ChunkPtr ClientWorld::FindChunk(const glm::ivec3& chunkCoords) const {
    auto it = chunks.find(chunkCoords);
    return it != chunks.end() ? it->second : nullptr;
}

ClientWorld::ChunkPtr ClientWorld::FindChunkShared(const glm::ivec3& chunkCoords) const {
    std::shared_lock<std::shared_mutex> lock(chunksMutex);
    return FindChunk(chunkCoords);
}

bool ClientWorld::GetBlock(int x, int y, int z, Block& block) const {
    auto chunk = FindChunkShared(Coords::ChunkOf({ x, y, z }));
    if (!chunk || !chunk->IsLoaded()) return false;
    const glm::ivec3 local = Coords::LocalOf({ x, y, z });
    block = chunk->GetBlock(local.x, local.y, local.z);
    return true;
}

void ClientWorld::SetBlock(int x, int y, int z, const Block& block) {
    const glm::ivec3 chunkCoords = Coords::ChunkOf({ x, y, z });
    auto chunk = FindChunk(chunkCoords);
    if (!chunk || !chunk->IsLoaded() || chunk->IsRemoved()) return;

    const glm::ivec3 local = Coords::LocalOf({ x, y, z });
    if (chunk->GetBlock(local.x, local.y, local.z) == block) return;
    chunk->SetBlock(local.x, local.y, local.z, block);

    Coords::ForEachChunkTouching({ x, y, z }, [&](const glm::ivec3& coords) {
        auto touched = coords == chunkCoords ? chunk : FindChunk(coords);
        if (touched && touched->IsLoaded()) RequestRebuild(touched);
    });
}

void ClientWorld::ApplyChunk(const glm::ivec3& coords, const Chunk::Blocks& blocks, const Chunk::Light& light) {
    auto chunk = FindChunk(coords);
    if (!chunk) {
        chunk = std::make_shared<ClientChunk>(coords);
        std::unique_lock<std::shared_mutex> lock(chunksMutex);
        chunks.emplace(coords, chunk);
    }
    if (!chunk->IsLoaded()) loadedCount++;
    chunk->SetLightData(light);
    chunk->SetGenerated(blocks);
    RequestRebuild(chunk);
    RequestNeighbourRebuilds(coords);
}

void ClientWorld::ApplyLight(const glm::ivec3& coords, const Chunk::Light& light) {
    auto chunk = FindChunk(coords);
    if (!chunk || !chunk->IsLoaded()) return;
    chunk->SetLightData(light, &changedCells);
    if (changedCells.empty()) return;

    // Rebuild this chunk, and each neighbour whose mesh samples a changed border cell.
    std::unordered_set<glm::ivec3, Coords::IVec3Hash> touched;
    const glm::ivec3 origin = coords * Chunk::CHUNK_SIZE;
    constexpr int S = Chunk::CHUNK_SIZE;
    for (int i : changedCells) {
        const glm::ivec3 local(i % S, (i / S) % S, i / (S * S));
        Coords::ForEachChunkTouching(origin + local, [&](const glm::ivec3& c) { touched.insert(c); });
    }
    for (const auto& c : touched) {
        auto t = FindChunk(c);
        if (t && t->IsLoaded()) RequestRebuild(t);
    }
}

void ClientWorld::UnloadChunk(const glm::ivec3& coords) {
    auto chunk = FindChunk(coords);
    if (!chunk) return;
    if (chunk->IsLoaded()) loadedCount--;
    RemoveChunk(coords);
    // Neighbours now border nothing there; they keep their meshes (unloaded chunks are
    // at the edge of the view, where the difference doesn't show).
}

void ClientWorld::RemoveChunk(const glm::ivec3& coords) {
    std::unique_lock<std::shared_mutex> lock(chunksMutex);
    auto it = chunks.find(coords);
    if (it == chunks.end()) return;
    it->second->MarkRemoved();
    it->second->ReleaseGpu();
    chunks.erase(it);
}

void ClientWorld::RebuildAllChunks() {
    for (auto& [coords, chunk] : chunks)
        if (chunk->IsLoaded()) RequestRebuild(chunk);
}

void ClientWorld::Update(const glm::vec3& cameraPosition) {
    if (loadOffsets.empty()) return;
    const glm::ivec3 center = Coords::ChunkOf(glm::ivec3(glm::floor(cameraPosition)));
    if (hasCenter && center == lastCenter) return;
    lastCenter = center;
    hasCenter = true;
    RefreshPlaceholders(center);
}

// The server sends every chunk within view distance of the player. Until one arrives
// it's a placeholder here, so its neighbours wait for it before meshing (otherwise
// every chunk would be meshed again as each of its 26 neighbours arrives). Only
// placeholders are managed here: loaded chunks stay until the server unloads them.
void ClientWorld::RefreshPlaceholders(const glm::ivec3& center) {
    std::vector<glm::ivec3> dropped;
    {
        std::unique_lock<std::shared_mutex> lock(chunksMutex);
        for (auto it = chunks.begin(); it != chunks.end();) {
            const glm::ivec3 offset = it->first - center;
            const glm::ivec3 d = glm::abs(offset);
            const bool inRange = d.x <= renderDistance && d.z <= renderDistance && offset.y >= -verticalRenderDistance &&
                                 offset.y < verticalRenderDistance;
            if (!inRange && !it->second->IsLoaded()) {
                dropped.push_back(it->first);
                it->second->MarkRemoved();
                it = chunks.erase(it);
            }
            else {
                ++it;
            }
        }
        for (const auto& offset : loadOffsets) {
            const glm::ivec3 coords = center + offset;
            if (chunks.find(coords) == chunks.end()) chunks.emplace(coords, std::make_shared<ClientChunk>(coords));
        }
    }
    // Loaded neighbours of dropped placeholders may have been waiting on them.
    for (const auto& coords : dropped) RequestNeighbourRebuilds(coords);
}

void ClientWorld::RequestRebuild(const ChunkPtr& chunk) {
    if (!chunk->MarkDirty()) return; // already queued
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        rebuildQueue.push_back(chunk);
    }
    queueCv.notify_one();
}

void ClientWorld::RequestNeighbourRebuilds(const glm::ivec3& chunkCoords) {
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                auto neighbour = FindChunk(chunkCoords + glm::ivec3(dx, dy, dz));
                if (neighbour && neighbour->IsLoaded()) RequestRebuild(neighbour);
            }
}

bool ClientWorld::BuildChunkMesh(ClientChunk& chunk) {
    Chunk::Neighbours neighbours;
    {
        std::shared_lock<std::shared_mutex> lock(chunksMutex);
        const glm::ivec3 coords = chunk.GetCoords();
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dz = -1; dz <= 1; dz++) {
                    if (dx == 0 && dy == 0 && dz == 0) continue;
                    auto neighbour = FindChunk(coords + glm::ivec3(dx, dy, dz));
                    // A neighbour still waiting for its data will change this mesh
                    // (culling, AO and light at the border) once it arrives, and then
                    // requests a rebuild of this one. Skip the build until then.
                    if (neighbour && !neighbour->IsLoaded() && !neighbour->IsRemoved()) return false;
                    neighbours[Chunk::NeighbourIndex(dx, dy, dz)] = std::move(neighbour);
                }
    }
    chunk.BuildMesh(neighbours);
    return true;
}

void ClientWorld::WorkerLoop() {
    while (true) {
        ChunkPtr chunk;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [this] { return stopping || !rebuildQueue.empty(); });
            if (stopping) return;
            chunk = std::move(rebuildQueue.front());
            rebuildQueue.pop_front();
        }
        // Clear before building so a change made during the build re-queues it.
        chunk->ClearDirty();
        if (chunk->IsRemoved() || !chunk->IsLoaded()) continue;
        BuildChunkMesh(*chunk);
    }
}

void ClientWorld::Render(Shader& shader, const glm::mat4& view, const glm::mat4& projection, float time) {
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

void ClientWorld::RenderWater(Shader& shader, const glm::mat4& view, const glm::mat4& projection, float time, const glm::vec3& cameraPosition) {
    // Front to back: water is drawn opaque (the shader refracts the scene copied behind
    // it), so nearer surfaces hide farther ones before they're shaded.
    std::vector<std::pair<float, ClientChunk*>> order;
    for (auto& [coords, chunk] : chunks) {
        if (!chunk->IsLoaded() || !chunk->HasWater()) continue;
        const glm::vec3 centre = glm::vec3(coords * Chunk::CHUNK_SIZE) + glm::vec3(Chunk::CHUNK_SIZE * 0.5f);
        const glm::vec3 d = centre - cameraPosition;
        order.emplace_back(glm::dot(d, d), chunk.get());
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    shader.Use();
    shader.SetUniform("time", time);
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    for (const auto& [distance, chunk] : order) {
        shader.SetUniform("model", glm::translate(glm::mat4(1.0f), glm::vec3(chunk->GetCoords() * Chunk::CHUNK_SIZE)));
        chunk->DrawWater();
    }
}
