#include <iostream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "World.h"
#include "Shader.h"

inline static int rem(int a, int b) {
    return ((a % b) + b) % b;
}

World::World(TerrainGenerator* terrainGenerator) : terrainGenerator(terrainGenerator), running(true) {}

World::World(const World& other) : terrainGenerator(other.terrainGenerator), running(true) {}

glm::ivec3 World::WorldToChunkCoordinates(glm::vec3 position) {
    return World::WorldToChunkCoordinates((int)position.x, (int)position.y, (int)position.z);
}

glm::ivec3 World::WorldToChunkCoordinates(int x, int y, int z) {
    return glm::ivec3(std::floor((float)x / Chunk::CHUNK_SIZE),
        std::floor((float)y / Chunk::CHUNK_SIZE),
        std::floor((float)z / Chunk::CHUNK_SIZE));
}

glm::ivec3 World::WorldToBlockCoordinates(int x, int y, int z) {
    return glm::ivec3(rem(x, Chunk::CHUNK_SIZE), rem(y, Chunk::CHUNK_SIZE), rem(z, Chunk::CHUNK_SIZE));
}

bool World::GetChunk(Chunk*& chunk, int chunkX, int chunkY, int chunkZ)
{
    auto key = std::make_tuple(chunkX, chunkY, chunkZ);
    if (chunks.find(key) == chunks.end()) {
        return false;
    }

    chunk = chunks[key].get();
    return true;
}

std::unique_ptr<Chunk> World::GetChunk(int chunkX, int chunkY, int chunkZ)
{
    auto key = std::make_tuple(chunkX, chunkY, chunkZ);
    if (chunks.find(key) != chunks.end()) {
        return std::unique_ptr<Chunk>(chunks[key].get());
    }
    return nullptr;
}

bool World::GetBlock(int x, int y, int z, Block& block) {
    auto chunkCoords = WorldToChunkCoordinates(x, y, z);
    auto blockCoords = WorldToBlockCoordinates(x, y, z);
    Chunk* chunk = nullptr;
    if (GetChunk(chunk, chunkCoords.x, chunkCoords.y, chunkCoords.z)) {
        block = chunk->GetBlock(blockCoords.x, blockCoords.y, blockCoords.z);
        return true;
    }
    return false;
}

bool World::GetBlockCulls(int x, int y, int z) {
    auto chunkCoords = WorldToChunkCoordinates(x, y, z);
    auto blockCoords = WorldToBlockCoordinates(x, y, z);
    auto pChunk = GetChunk(chunkCoords.x, chunkCoords.y, chunkCoords.z);
    if (pChunk) {
        return pChunk->GetBlockCulls(blockCoords.x, blockCoords.y, blockCoords.z);
    }
    return false;
}

void World::SetBlock(int x, int y, int z, Block block)
{
    auto chunkCoords = WorldToChunkCoordinates(x, y, z);
    auto blockCoords = WorldToBlockCoordinates(x, y, z);
    auto pChunk = GetChunk(chunkCoords.x, chunkCoords.y, chunkCoords.z);
    if (pChunk) {
        pChunk->SetBlock(blockCoords.x, blockCoords.y, blockCoords.z, block);
        pChunk->SetNeedsRebuilding(true);
        UpdateAdjacentChunks(x, y, z);
    }
}

void World::SetBlock(int x, int y, int z, BlockType type) {
    auto chunkCoords = WorldToChunkCoordinates(x, y, z);
    auto blockCoords = WorldToBlockCoordinates(x, y, z);
    auto pChunk = GetChunk(chunkCoords.x, chunkCoords.y, chunkCoords.z);
    if (pChunk) {
        pChunk->SetBlock(blockCoords.x, blockCoords.y, blockCoords.z, type);
        pChunk->SetNeedsRebuilding(true);
        UpdateAdjacentChunks(x, y, z);
    }
}

void World::SetBlock(int x, int y, int z, EdgeData edges) {
    auto chunkCoords = WorldToChunkCoordinates(x, y, z);
    auto blockCoords = WorldToBlockCoordinates(x, y, z);
    auto pChunk = GetChunk(chunkCoords.x, chunkCoords.y, chunkCoords.z);
    if (pChunk) {
        pChunk->SetBlock(blockCoords.x, blockCoords.y, blockCoords.z, edges);
        pChunk->SetNeedsRebuilding(true);
        UpdateAdjacentChunks(x, y, z);
    }
}

void World::UpdateAdjacentChunks(int x, int y, int z) {
    glm::ivec3 blockCoords = WorldToBlockCoordinates(x, y, z);

    std::array<glm::ivec3, 3> chunksToRebuild{};
    int s = 0;

    if (blockCoords.x == 0) { chunksToRebuild[s] = WorldToChunkCoordinates(x - 1, y, z); s++; }
    if (blockCoords.x == Chunk::CHUNK_SIZE - 1) { chunksToRebuild[s] = WorldToChunkCoordinates(x + 1, y, z); s++; }
    
    if (blockCoords.y == 0) { chunksToRebuild[s] = WorldToChunkCoordinates(x, y - 1, z); s++; }
    if (blockCoords.y == Chunk::CHUNK_SIZE - 1) { chunksToRebuild[s] = WorldToChunkCoordinates(x, y + 1, z); s++; }
    
    if (blockCoords.z == 0) { chunksToRebuild[s] = WorldToChunkCoordinates(x, y, z - 1); s++; }
    if (blockCoords.z == Chunk::CHUNK_SIZE - 1) { chunksToRebuild[s] = WorldToChunkCoordinates(x, y, z + 1); s++; }

    for (int i = 0; i < s; i++)
    {
        auto& chunkCoords = chunksToRebuild[i];
        auto pChunk = GetChunk(chunkCoords.x, chunkCoords.y, chunkCoords.z);
        if (pChunk != NULL) {
            pChunk->SetNeedsRebuilding(true);
        }
    }
}

void World::UpdateAdjacentChunks(Chunk& chunk) {
    auto coords = chunk.GetCoords();

    std::array<glm::ivec3, 6> chunksToRebuild{ 
        glm::ivec3(coords.x - 1, coords.y, coords.z),
        glm::ivec3(coords.x + 1, coords.y, coords.z),
        glm::ivec3(coords.x, coords.y - 1, coords.z),
        glm::ivec3(coords.x, coords.y + 1, coords.z),
        glm::ivec3(coords.x, coords.y, coords.z - 1),
        glm::ivec3(coords.x, coords.y, coords.z + 1),
    };

    for (int i = 0; i < chunksToRebuild.size(); i++)
    {
        auto& chunkCoords = chunksToRebuild[i];
        auto pChunk = GetChunk(chunkCoords.x, chunkCoords.y, chunkCoords.z);
        if (pChunk) {
            pChunk->SetNeedsRebuilding(true);
        }
    }
}

void World::WorldThread() {
    while (running) {
        UpdateLoadList();
        UpdateSetupList();
        UpdateRebuildList();
        UpdateFlagsList();
        UpdateUnloadList();

        // Sleep for a short duration to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void World::Update(Camera* camera) {
    glm::vec3 cameraPosition = camera->GetPosition();
    glm::vec3 cameraView = camera->GetDirection();

    if (m_cameraPosition != cameraPosition || m_cameraView != cameraView || m_forceVisibilityUpdate) {
        UpdateRenderList();
    }

    m_cameraPosition = cameraPosition;
    m_cameraView = cameraView;

    // Update the async chunker to find which chunks need to be loaded or unloaded
    UpdateAsyncChunker();
}

void World::UpdateAsyncChunker() {
    auto chunkCoords = WorldToChunkCoordinates(m_cameraPosition);

    for (auto iterator = chunks.begin(); iterator != chunks.end(); ++iterator) {
        std::unique_ptr<Chunk>& pChunk = (*iterator).second;
        auto coords = pChunk->GetCoords();

        int distX = abs((coords.x - chunkCoords.x));
        int distY = abs((coords.y - chunkCoords.y));
        int distZ = abs((coords.z - chunkCoords.z));

        

        if (distX > RENDER_DISTANCE + 1 || distY > RENDER_DISTANCE + 1 || distZ > RENDER_DISTANCE + 1) {
            const std::lock_guard<std::mutex> chunkUnloadLock(m_chunkUnloadQueueMutex);
            // If the chunk is outside the render distance, add it to the unload list
            if (pChunk->IsLoaded() && pChunk->IsSetup()) {
                m_chunkUnloadQueue.push(std::move(pChunk));
                chunks.erase(iterator);
            }
            //m_vpChunkUnloadList.push_back(pChunk);
        }
    }

    int x, z, dx, dy;
    x = z = dx = 0;
    dy = -1;
    int width = RENDER_DISTANCE * 2;
    int t = width;
    int maxI = t * t;
    for (int i = 0; i < maxI; i++) {
        if ((-width / 2 <= x) && (x <= width / 2) && (-width / 2 <= z) && (z <= width / 2)) {
            for (int y = -RENDER_DISTANCE; y < RENDER_DISTANCE; ++y) {
                int chunkX = x + chunkCoords.x;
                int chunkY = y + chunkCoords.y;
                int chunkZ = z + chunkCoords.z;
                auto key = std::make_tuple(chunkX, chunkY, chunkZ);
                auto search = chunks.find(key);
                if (search == chunks.end()) {
                    // If the chunk doesn't exist, create it and add it to the load list
                    std::lock_guard<std::mutex> chunkLoadLock(m_chunkLoadQueueMutex);
                    m_chunkLoadQueue.push(glm::ivec3(chunkX, chunkY, chunkZ));
                }
                else if (search->second->NeedsRebuilding()) {
                    // If the chunk exists and needs rebuilding, add it to the rebuild list
                    std::lock_guard<std::mutex> chunkRebuildLock(m_chunkRebuildQueueMutex);
                    m_chunkRebuildQueue.push(std::move(search->second));
                }
            }
        }
        if ((x == z) || ((x < 0) && (x == -z)) || ((x > 0) && (x == 1 - z))) {
            t = dx;
            dx = -dy;
            dy = t;
        }
        x += dx;
        z += dy;
    }

    /*for (int x = -RENDER_DISTANCE; x < RENDER_DISTANCE; ++x) {
        for (int y = -RENDER_DISTANCE; y < RENDER_DISTANCE; ++y) {
            for (int z = -RENDER_DISTANCE; z < RENDER_DISTANCE; ++z) {
                int chunkX = x + chunkCoords.x;
                int chunkY = y + chunkCoords.y;
                int chunkZ = z + chunkCoords.z;
                auto key = std::make_tuple(chunkX, chunkY, chunkZ);
                auto search = chunks.find(key);
                if (search == chunks.end()) {
                    chunks[key] = std::make_shared<Chunk>(this, chunkX, chunkY, chunkZ);
                    m_vpChunkLoadList.push_back(chunks[key]);
                }
                else if (!search->second->IsLoaded()) {
                    m_vpChunkLoadList.push_back(chunks[key]);
                }
            }
        }
    }*/
}

void World::UpdateLoadList() {
    int lNumOfChunksLoaded = 0;
    std::lock_guard<std::mutex> chunkLoadLock(m_chunkLoadQueueMutex);
    int count = 0;
    while (m_chunkLoadQueue.size() > 0 && (lNumOfChunksLoaded < ASYNC_NUM_CHUNKS_PER_FRAME)) {
        glm::ivec3 coords = m_chunkLoadQueue.front();
        m_chunkLoadQueue.pop();

        std::unique_ptr<Chunk> chunk = std::make_unique<Chunk>(this, coords.x, coords.y, coords.z);

        chunk->LoadChunk(terrainGenerator);
        lNumOfChunksLoaded++;
        m_forceVisibilityUpdate = true;

        m_vpChunkSetupList.push_back(std::move(chunk));

        count++;
    }

    m_chunkLoadQueue.empty();
}

void World::UpdateSetupList() {
    for (auto iterator = m_vpChunkSetupList.begin(); iterator != m_vpChunkSetupList.end(); ++iterator) {
        std::unique_ptr<Chunk>& pChunk = *iterator;
        if (!pChunk->IsSetup()) {
            pChunk->SetupChunk();
            // If we set up a chunk, add it to the list of chunks that need to be rebuilt
            std::lock_guard<std::mutex> chunkRebuildLock(m_chunkRebuildQueueMutex);
            UpdateAdjacentChunks(*pChunk);
            m_chunkRebuildQueue.push(std::move(pChunk));
            m_forceVisibilityUpdate = true;
        }
    }
    m_vpChunkSetupList.clear();
}

void World::UpdateRebuildList() {
    // Rebuild any chunks that are in the rebuild chunk list     
    while (!m_chunkRebuildQueue.empty()) {
        std::unique_ptr<Chunk> pChunk = std::move(m_chunkRebuildQueue.front());
        m_chunkRebuildQueue.pop();
        // If the chunk is not loaded, skip it
        if (pChunk->IsLoaded() && pChunk->IsSetup()) {
            pChunk->GenerateMesh();
            // If we rebuild a chunk, add it to the list of chunks that need their render flags updated                 
            // since we might now be empty or surrounded
            m_vpChunkUpdateFlagsList.push_back(std::move(pChunk));

            // Also add our neighbours since they might now be surrounded too (If we have neighbours)         
            /*auto coords = pChunk->GetCoords();
            auto pChunkXMinus = GetChunk(coords.x - 1, coords.y, coords.z);
            auto pChunkXPlus = GetChunk(coords.x + 1, coords.y, coords.z);
            auto pChunkYMinus = GetChunk(coords.x, coords.y - 1, coords.z);
            auto pChunkYPlus = GetChunk(coords.x, coords.y + 1, coords.z);
            auto pChunkZMinus = GetChunk(coords.x, coords.y, coords.z - 1);
            auto pChunkZPlus = GetChunk(coords.x, coords.y, coords.z + 1);
            if (pChunkXMinus != NULL) m_vpChunkUpdateFlagsList.push_back(std::move(pChunkXMinus));
            if (pChunkXPlus != NULL) m_vpChunkUpdateFlagsList.push_back(std::move(pChunkXPlus));
            if (pChunkYMinus != NULL) m_vpChunkUpdateFlagsList.push_back(std::move(pChunkYMinus));
            if (pChunkYPlus != NULL) m_vpChunkUpdateFlagsList.push_back(std::move(pChunkYPlus));
            if (pChunkZMinus != NULL) m_vpChunkUpdateFlagsList.push_back(std::move(pChunkZMinus));
            if (pChunkZPlus != NULL) m_vpChunkUpdateFlagsList.push_back(std::move(pChunkZPlus));*/

            m_forceVisibilityUpdate = true;
        }
    }
}

void World::UpdateFlagsList() {
    for (auto iterator = m_vpChunkUpdateFlagsList.begin(); iterator != m_vpChunkUpdateFlagsList.end(); ++iterator) {
        Chunk& pChunk = **iterator;
        if (pChunk.IsLoaded() && pChunk.IsSetup()) {
            pChunk.UpdateEmptyFullFlags();
        }
    }
    for (auto iterator = m_vpChunkUpdateFlagsList.begin(); iterator != m_vpChunkUpdateFlagsList.end(); ++iterator) {
        Chunk& pChunk = **iterator;
        if (pChunk.IsLoaded() && pChunk.IsSetup()) {
            int offsets[][3] = {
                { 1, 0, 0 },
                { -1, 0, 0 },
                { 0, 1, 0 },
                { 0, -1, 0 },
                { 0, 0, 1 },
                { 0, 0, -1 },
            };

            bool surrounded = true;

            for (int i = 0; i < 6; i++)
            {
                auto neighbour = GetChunk(offsets[i][0], offsets[i][1], offsets[i][2]);
                if (neighbour == NULL || !neighbour->IsFull()) {
                    surrounded = false;
                    break; 
                }
            }

            pChunk.SetIsSurrounded(surrounded);
        }
    }
    m_vpChunkUpdateFlagsList.clear();
}

void World::UpdateUnloadList() {
    //std::cout << "Unload list size: " << m_vpChunkUnloadList.size() << std::endl;

    while (!m_chunkUnloadQueue.empty()) {
        Chunk& pChunk = *m_chunkUnloadQueue.front();
        m_chunkUnloadQueue.pop();
        if (pChunk.IsLoaded()) {
            pChunk.UnloadChunk();
            auto coords = pChunk.GetCoords();

            m_forceVisibilityUpdate = true;
        }
    }
}

void World::UpdateRenderList() {
    // Clear the render list each frame BEFORE we do our tests to see what chunks should be rendered
    m_vpChunkRenderList.clear();
    for (auto iterator = chunks.begin(); iterator != chunks.end(); ++iterator) {
        Chunk& pChunk = *(*iterator).second.get();
        if (pChunk.IsLoaded()) {
            if (pChunk.IsSetup()) {
                if (!pChunk.IsEmpty() && !pChunk.IsSurrounded() || !pChunk.IsFull()) {
                    m_vpChunkRenderList.push_back(pChunk);
                }
            }
        }
    }

    //std::cout << "Render list size: " << m_vpChunkRenderList.size() << std::endl;
}

void World::Render(Shader& shader, glm::mat4& viewMatrix, glm::mat4& projectionMatrix, float frameWidth, float frameHeight, float time) {
    shader.Use();

    glm::mat4 model = glm::mat4(1.0f);

    shader.SetUniform("time", time);

    shader.SetUniform("model", model);
    shader.SetUniform("projection", projectionMatrix);

    for (auto iterator = m_vpChunkRenderList.begin(); iterator != m_vpChunkRenderList.end(); ++iterator) {
        Chunk& pChunk = *iterator;

        auto coords = pChunk.GetCoords();

        glm::mat4 newViewMatrix = glm::translate(viewMatrix, glm::vec3(coords.x * Chunk::CHUNK_SIZE, coords.y * Chunk::CHUNK_SIZE, coords.z * Chunk::CHUNK_SIZE));
        shader.SetUniform("view", newViewMatrix);

        pChunk.Render(shader);
    }
}

void World::Stop() {
    running = false;
}

void World::RebuildAllChunks()
{
    for (auto iterator = chunks.begin(); iterator != chunks.end(); ++iterator) {
        Chunk* pChunk = (*iterator).second.get();
        pChunk->SetNeedsRebuilding(true);
    }
}