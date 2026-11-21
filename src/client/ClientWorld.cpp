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

bool ClientWorld::GetLight(int x, int y, int z, int& blockLight, int& skyLight) const {
    auto chunk = FindChunkShared(Coords::ChunkOf({ x, y, z }));
    if (!chunk || !chunk->IsLoaded()) return false;
    const glm::ivec3 local = Coords::LocalOf({ x, y, z });
    const uint8_t v = chunk->GetReceivedLight(local.x, local.y, local.z);
    blockLight = v & 0x0F;
    skyLight = v >> 4;
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
    drawListDirty = true; // it may be in the draw list
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
        if (!BuildChunkMesh(*chunk)) continue;
        std::lock_guard<std::mutex> lock(uploadMutex);
        uploadQueue.push_back(std::move(chunk));
    }
}

void ClientWorld::PrepareDraw() {
    {
        std::lock_guard<std::mutex> lock(uploadMutex);
        uploading.swap(uploadQueue);
    }
    for (const auto& chunk : uploading)
        if (chunk->UploadPendingMesh()) drawListDirty = true;
    uploading.clear();

    if (!drawListDirty) return;
    drawListDirty = false;
    drawList.clear();
    for (auto& [coords, chunk] : chunks)
        if (chunk->HasGeometry() || chunk->HasWater()) drawList.push_back({ chunk.get(), glm::vec3(coords * Chunk::CHUNK_SIZE), 0.0f });
}

void ClientWorld::Render(Shader& shader, const glm::mat4& view, const glm::mat4& projection, float time) {
    PrepareDraw();

    // Frustum planes (pointing inward) from the combined matrix: a point p is inside
    // when dot(plane.xyz, p) + plane.w >= 0 for all six.
    const glm::mat4 m = projection * view;
    auto row = [&](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
    const glm::vec4 planes[6] = { row(3) + row(0), row(3) - row(0), row(3) + row(1), row(3) - row(1), row(3) + row(2), row(3) - row(2) };
    const glm::vec3 camera = glm::vec3(glm::inverse(view)[3]);

    // A chunk's mesh stays inside its cube, give or take a block (see Block.cpp).
    constexpr float kMargin = 1.0f;
    constexpr float kSize = Chunk::CHUNK_SIZE;
    auto inFrustum = [&](const glm::vec3& origin) {
        const glm::vec3 lo = origin - kMargin, hi = origin + kSize + kMargin;
        for (const auto& plane : planes) {
            // The box corner furthest along the plane's normal.
            const glm::vec3 corner(plane.x >= 0.0f ? hi.x : lo.x, plane.y >= 0.0f ? hi.y : lo.y, plane.z >= 0.0f ? hi.z : lo.z);
            if (glm::dot(glm::vec3(plane), corner) + plane.w < 0.0f) return false;
        }
        return true;
    };
    visible.clear();
    visibleWater = false;
    auto see = [&](ClientChunk* chunk, const glm::vec3& origin) {
        if (!chunk->HasGeometry() && !chunk->HasWater()) return;
        const glm::vec3 d = origin + kSize * 0.5f - camera;
        visible.push_back({ chunk, origin, glm::dot(d, d) });
        visibleWater = visibleWater || chunk->HasWater();
    };

    // Which chunks can be seen at all: a flood out from the camera's chunk, through
    // the frustum, only on through a chunk from the face it was entered by to faces it
    // connects to through open cells (ClientChunk::GetConnectivity), and never back
    // toward the camera. Caves under solid ground, which the frustum alone would keep,
    // are left out. (Chunks not here yet count as open.)
    const glm::ivec3 home = Coords::ChunkOf(glm::ivec3(glm::floor(camera)));
    const int reachH = renderDistance + 2, reachV = std::max(verticalRenderDistance, renderDistance) + 2;
    const int sizeH = 2 * reachH + 1, sizeV = 2 * reachV + 1;
    floodSeen.assign(static_cast<size_t>(sizeH) * sizeH * sizeV, 0);
    auto seenIndex = [&](const glm::ivec3& c) -> int {
        const glm::ivec3 o = c - home;
        if (std::abs(o.x) > reachH || std::abs(o.z) > reachH || std::abs(o.y) > reachV) return -1;
        return (o.x + reachH) + sizeH * ((o.z + reachH) + sizeH * (o.y + reachV));
    };
    floodQueue.clear();
    floodQueue.push_back({ home, -1, 0 });
    floodSeen[seenIndex(home)] = 1;
    for (size_t head = 0; head < floodQueue.size(); head++) {
        const FloodStep step = floodQueue[head];
        const auto it = chunks.find(step.coords);
        ClientChunk* chunk = it != chunks.end() && it->second->IsLoaded() ? it->second.get() : nullptr;
        const glm::vec3 origin(step.coords * Chunk::CHUNK_SIZE);
        if (chunk) see(chunk, origin);
        const uint64_t connectivity = chunk ? chunk->GetConnectivity() : ~0ull;
        for (int face = 0; face < 6; face++) {
            const int opposite = face ^ 1;
            if (step.directions & (1u << opposite)) continue; // back toward the camera
            if (step.entry >= 0 && !ClientChunk::Connects(connectivity, step.entry, face)) continue;
            const glm::ivec3 next = step.coords + glm::ivec3(ClientChunk::kFaceDirections[face][0], ClientChunk::kFaceDirections[face][1],
                                                             ClientChunk::kFaceDirections[face][2]);
            const int index = seenIndex(next);
            if (index < 0 || floodSeen[index]) continue;
            if (!inFrustum(glm::vec3(next * Chunk::CHUNK_SIZE))) continue;
            floodSeen[index] = 1;
            floodQueue.push_back({ next, static_cast<int8_t>(opposite), static_cast<uint8_t>(step.directions | (1u << face)) });
        }
    }
    std::sort(visible.begin(), visible.end(), [](const DrawItem& a, const DrawItem& b) { return a.distance < b.distance; });

    shader.Use();
    shader.SetUniform("time", time);
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    const GLint modelLocation = shader.GetLocation("model");
    glm::mat4 model(1.0f);
    for (const auto& item : visible) {
        if (!item.chunk->HasGeometry()) continue;
        model[3] = glm::vec4(item.origin, 1.0f);
        glUniformMatrix4fv(modelLocation, 1, GL_FALSE, &model[0][0]);
        item.chunk->Draw();
    }
}

void ClientWorld::RenderWater(Shader& shader, const glm::mat4& view, const glm::mat4& projection, float time) {
    // Front to back: water is drawn opaque (the shader refracts the scene copied behind
    // it), so nearer surfaces hide farther ones before they're shaded.
    shader.Use();
    shader.SetUniform("time", time);
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    const GLint modelLocation = shader.GetLocation("model");
    glm::mat4 model(1.0f);
    for (const auto& item : visible) {
        if (!item.chunk->HasWater()) continue;
        model[3] = glm::vec4(item.origin, 1.0f);
        glUniformMatrix4fv(modelLocation, 1, GL_FALSE, &model[0][0]);
        item.chunk->DrawWater();
    }
}
