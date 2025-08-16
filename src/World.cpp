#include <iostream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "World.h"
#include "Shader.h"

inline static int rem(int a, int b) {
    return ((a % b) + b) % b;
}

World::World(TerrainGenerator* terrainGenerator)
    : terrainGenerator(terrainGenerator), running(true), m_forceVisibilityUpdate(false) {
}

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

Chunk* World::GetChunk(int chunkX, int chunkY, int chunkZ)
{
    std::lock_guard<std::mutex> lock(chunksMutex);
    auto key = std::make_tuple(chunkX, chunkY, chunkZ);
    if (chunks.find(key) != chunks.end()) {
        return chunks[key].get();
    }
    return nullptr;
}

bool World::GetBlock(int x, int y, int z, Block& block) {
    auto chunkCoords = WorldToChunkCoordinates(x, y, z);
    auto blockCoords = WorldToBlockCoordinates(x, y, z);
    Chunk* chunk = GetChunk(chunkCoords.x, chunkCoords.y, chunkCoords.z);
    if (chunk) {
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
        // Immediately update the block
        pChunk->SetBlock(blockCoords.x, blockCoords.y, blockCoords.z, block);

        // Always rebuild the current chunk immediately on main thread
        pChunk->GenerateMesh();

        // Check if this is an edge block that affects neighbors
        std::vector<Chunk*> neighborsToRebuild;

        if (blockCoords.x == 0) {
            auto neighbor = GetChunk(chunkCoords.x - 1, chunkCoords.y, chunkCoords.z);
            if (neighbor && neighbor->IsLoaded() && neighbor->IsSetup()) {
                neighborsToRebuild.push_back(neighbor);
            }
        }
        if (blockCoords.x == Chunk::CHUNK_SIZE - 1) {
            auto neighbor = GetChunk(chunkCoords.x + 1, chunkCoords.y, chunkCoords.z);
            if (neighbor && neighbor->IsLoaded() && neighbor->IsSetup()) {
                neighborsToRebuild.push_back(neighbor);
            }
        }
        if (blockCoords.y == 0) {
            auto neighbor = GetChunk(chunkCoords.x, chunkCoords.y - 1, chunkCoords.z);
            if (neighbor && neighbor->IsLoaded() && neighbor->IsSetup()) {
                neighborsToRebuild.push_back(neighbor);
            }
        }
        if (blockCoords.y == Chunk::CHUNK_SIZE - 1) {
            auto neighbor = GetChunk(chunkCoords.x, chunkCoords.y + 1, chunkCoords.z);
            if (neighbor && neighbor->IsLoaded() && neighbor->IsSetup()) {
                neighborsToRebuild.push_back(neighbor);
            }
        }
        if (blockCoords.z == 0) {
            auto neighbor = GetChunk(chunkCoords.x, chunkCoords.y, chunkCoords.z - 1);
            if (neighbor && neighbor->IsLoaded() && neighbor->IsSetup()) {
                neighborsToRebuild.push_back(neighbor);
            }
        }
        if (blockCoords.z == Chunk::CHUNK_SIZE - 1) {
            auto neighbor = GetChunk(chunkCoords.x, chunkCoords.y, chunkCoords.z + 1);
            if (neighbor && neighbor->IsLoaded() && neighbor->IsSetup()) {
                neighborsToRebuild.push_back(neighbor);
            }
        }

        // Rebuild all affected neighbors immediately on main thread
        for (auto* neighbor : neighborsToRebuild) {
            neighbor->GenerateMesh();
        }
    }
}



void World::SetBlocksBatch(const std::vector<std::tuple<int, int, int, Block>>& blocks)
{
    if (blocks.empty()) return;

    // Group blocks by chunk
    std::map<std::tuple<int, int, int>, std::vector<std::tuple<int, int, int, Block>>> blocksByChunk;
    std::set<std::tuple<int, int, int>> allAffectedChunks;

    for (const auto& b : blocks) {
        int x = std::get<0>(b);
        int y = std::get<1>(b);
        int z = std::get<2>(b);

        auto chunkCoords = WorldToChunkCoordinates(x, y, z);
        auto blockCoords = WorldToBlockCoordinates(x, y, z);
        auto key = std::make_tuple(chunkCoords.x, chunkCoords.y, chunkCoords.z);

        blocksByChunk[key].push_back(b);
        allAffectedChunks.insert(key);

        // Add adjacent chunks if block is on edge
        if (blockCoords.x == 0)
            allAffectedChunks.insert(std::make_tuple(chunkCoords.x - 1, chunkCoords.y, chunkCoords.z));
        if (blockCoords.x == Chunk::CHUNK_SIZE - 1)
            allAffectedChunks.insert(std::make_tuple(chunkCoords.x + 1, chunkCoords.y, chunkCoords.z));
        if (blockCoords.y == 0)
            allAffectedChunks.insert(std::make_tuple(chunkCoords.x, chunkCoords.y - 1, chunkCoords.z));
        if (blockCoords.y == Chunk::CHUNK_SIZE - 1)
            allAffectedChunks.insert(std::make_tuple(chunkCoords.x, chunkCoords.y + 1, chunkCoords.z));
        if (blockCoords.z == 0)
            allAffectedChunks.insert(std::make_tuple(chunkCoords.x, chunkCoords.y, chunkCoords.z - 1));
        if (blockCoords.z == Chunk::CHUNK_SIZE - 1)
            allAffectedChunks.insert(std::make_tuple(chunkCoords.x, chunkCoords.y, chunkCoords.z + 1));
    }

    // Step 1: Update all blocks WITHOUT rebuilding meshes
    std::vector<Chunk*> chunksToUpdate;
    for (const auto& pair : blocksByChunk) {
        auto chunkKey = pair.first;
        auto pChunk = GetChunk(std::get<0>(chunkKey), std::get<1>(chunkKey), std::get<2>(chunkKey));

        if (pChunk && pChunk->IsLoaded()) {
            for (const auto& b : pair.second) {
                int x = std::get<0>(b);
                int y = std::get<1>(b);
                int z = std::get<2>(b);
                Block block = std::get<3>(b);

                auto blockCoords = WorldToBlockCoordinates(x, y, z);
                pChunk->SetBlock(blockCoords.x, blockCoords.y, blockCoords.z, block);
            }
            chunksToUpdate.push_back(pChunk);
        }
    }

    // Step 2: Update flags for chunks that had blocks changed
    for (auto* pChunk : chunksToUpdate) {
        pChunk->UpdateEmptyFullFlags();
    }

    // Step 3: Update surrounded flags for all affected chunks (including neighbors)
    for (const auto& chunkCoords : allAffectedChunks) {
        auto pChunk = GetChunk(std::get<0>(chunkCoords), std::get<1>(chunkCoords), std::get<2>(chunkCoords));
        if (pChunk && pChunk->IsLoaded() && pChunk->IsSetup()) {
            // Update this chunk's flags if we haven't already
            if (std::find(chunksToUpdate.begin(), chunksToUpdate.end(), pChunk) == chunksToUpdate.end()) {
                pChunk->UpdateEmptyFullFlags();
            }
            pChunk->UpdateChunkSurroundedFlag();
        }
    }

    // Step 4: Generate meshes for all affected chunks
    for (const auto& chunkCoords : allAffectedChunks) {
        auto pChunk = GetChunk(std::get<0>(chunkCoords), std::get<1>(chunkCoords), std::get<2>(chunkCoords));
        if (pChunk && pChunk->IsLoaded() && pChunk->IsSetup()) {
            pChunk->GenerateMesh();
        }
    }

    // Step 5: Force visibility and render list updates
    m_forceVisibilityUpdate = true;
    // Force immediate update of visibility list
    UpdateVisibilityList();
    UpdateRenderList();
}

void World::SetBlock(int x, int y, int z, BlockType type)
{
    Block block;
    block.type = type;
    block.edgeData.MakeFull();
    SetBlock(x, y, z, block);
}

void World::SetBlock(int x, int y, int z, EdgeData edges) {
    Block block;
    block.type = BlockType::DIRT;
    block.edgeData = edges;
    SetBlock(x, y, z, block);
}

void World::ProcessPendingModifications() {
    std::vector<BlockModification> mods;
    {
        std::lock_guard<std::mutex> lock(modificationsMutex);
        mods = std::move(pendingModifications);
        pendingModifications.clear();
    }

    std::set<std::tuple<int, int, int>> chunksToRebuild;

    for (const auto& mod : mods) {
        auto chunkCoords = WorldToChunkCoordinates(mod.x, mod.y, mod.z);
        auto blockCoords = WorldToBlockCoordinates(mod.x, mod.y, mod.z);
        auto pChunk = GetChunk(chunkCoords.x, chunkCoords.y, chunkCoords.z);

        if (pChunk) {
            pChunk->SetBlock(blockCoords.x, blockCoords.y, blockCoords.z, mod.block);
            chunksToRebuild.insert(std::make_tuple(chunkCoords.x, chunkCoords.y, chunkCoords.z));

            // Mark adjacent chunks if on edge
            if (blockCoords.x == 0) chunksToRebuild.insert(std::make_tuple(chunkCoords.x - 1, chunkCoords.y, chunkCoords.z));
            if (blockCoords.x == Chunk::CHUNK_SIZE - 1) chunksToRebuild.insert(std::make_tuple(chunkCoords.x + 1, chunkCoords.y, chunkCoords.z));
            if (blockCoords.y == 0) chunksToRebuild.insert(std::make_tuple(chunkCoords.x, chunkCoords.y - 1, chunkCoords.z));
            if (blockCoords.y == Chunk::CHUNK_SIZE - 1) chunksToRebuild.insert(std::make_tuple(chunkCoords.x, chunkCoords.y + 1, chunkCoords.z));
            if (blockCoords.z == 0) chunksToRebuild.insert(std::make_tuple(chunkCoords.x, chunkCoords.y, chunkCoords.z - 1));
            if (blockCoords.z == Chunk::CHUNK_SIZE - 1) chunksToRebuild.insert(std::make_tuple(chunkCoords.x, chunkCoords.y, chunkCoords.z + 1));
        }
    }

    // Add chunks to rebuild list
    if (!chunksToRebuild.empty()) {
        std::lock_guard<std::mutex> lock(rebuildListMutex);
        for (const auto& coords : chunksToRebuild) {
            auto pChunk = GetChunk(std::get<0>(coords), std::get<1>(coords), std::get<2>(coords));
            if (pChunk) {
                pChunk->SetNeedsRebuilding(true);
                m_vpChunkRebuildList.push_back(pChunk);
            }
        }
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

void World::UpdateAdjacentChunks(Chunk* chunk) {
    auto coords = chunk->GetCoords();

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

// Runs on a separate thread
void World::WorldThread() {
    while (running) {
        {
            std::unique_lock<std::mutex> lock(workMutex);
            workAvailable.wait_for(lock, std::chrono::milliseconds(20), [this] {
                // Check if there's work
                std::lock_guard<std::mutex> modLock(modificationsMutex);
                return !pendingModifications.empty() || !running;
            });
        }

        if (!running) break;

        ProcessPendingModifications();
        UpdateLoadList();
        UpdateSetupList();
        UpdateRebuildList();
        UpdateFlagsList();
        UpdateVisibilityList();
    }
}

void World::Update(Camera* camera) {
    glm::vec3 cameraPosition = camera->GetPosition();
    glm::vec3 cameraView = camera->GetDirection();

    UpdateAsyncChunker();

    if (m_cameraPosition != cameraPosition || m_cameraView != cameraView || m_forceVisibilityUpdate) {
        UpdateRenderList();
    }
    m_cameraPosition = cameraPosition;
    m_cameraView = cameraView;
}

void World::UpdateAsyncChunker() {
    auto chunkCoords = WorldToChunkCoordinates(m_cameraPosition);

    // Check for chunks to unload
    {
        std::lock_guard<std::mutex> chunkLock(chunksMutex);

        std::vector<std::tuple<int, int, int>> tempUnloadList;

        for (auto iterator = chunks.begin(); iterator != chunks.end(); ++iterator) {
            Chunk* pChunk = (*iterator).second.get();
            auto coords = pChunk->GetCoords();

            int distX = abs((coords.x - chunkCoords.x));
            int distY = abs((coords.y - chunkCoords.y));
            int distZ = abs((coords.z - chunkCoords.z));

            if (distX > RENDER_DISTANCE + 1 || distY > RENDER_DISTANCE + 1 || distZ > RENDER_DISTANCE + 1) {
                pChunk->UnloadChunk();
                m_vpChunkUnloadedList.push_back(std::move((*iterator).second));
                tempUnloadList.push_back((*iterator).first);
            }
        }

        for (auto iterator = tempUnloadList.begin(); iterator != tempUnloadList.end(); ++iterator) {
            chunks.erase(*iterator);
        }
    }

    // Load new chunks
    int x, z, dx, dy;
    x = z = dx = 0;
    dy = -1;
    int width = RENDER_DISTANCE * 2;
    int t = width;
    int maxI = t * t;

    std::lock_guard<std::mutex> chunkLock(chunksMutex);
    std::lock_guard<std::mutex> loadLock(loadListMutex);
    std::lock_guard<std::mutex> rebuildLock(rebuildListMutex);

    for (int i = 0; i < maxI; i++) {
        if ((-width / 2 <= x) && (x <= width / 2) && (-width / 2 <= z) && (z <= width / 2)) {
            for (int y = -RENDER_DISTANCE; y < RENDER_DISTANCE; ++y) {
                int chunkX = x + chunkCoords.x;
                int chunkY = y + chunkCoords.y;
                int chunkZ = z + chunkCoords.z;
                auto key = std::make_tuple(chunkX, chunkY, chunkZ);
                auto search = chunks.find(key);
                if (search == chunks.end()) {
                    if (!m_vpChunkUnloadedList.empty()) {
                        chunks[key] = std::move(m_vpChunkUnloadedList.back());
                        chunks[key]->Reset(chunkX, chunkY, chunkZ);
                        m_vpChunkUnloadedList.pop_back();
                    }
                    else {
                        chunks[key] = std::unique_ptr<Chunk>(new Chunk(this, chunkX, chunkY, chunkZ));
                    }
                    
                    m_vpChunkLoadList.push_back(chunks[key].get());
                }
                else {
                    if (!search->second->IsLoaded()) {
                        m_vpChunkLoadList.push_back(chunks[key].get());
                    }
                    else {
                        if (search->second->NeedsRebuilding()) {
                            m_vpChunkRebuildList.push_back(chunks[key].get());
                        }
                    }
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
}


void World::UpdateLoadList() {
    std::vector<Chunk*> tempLoadList;
    {
        std::lock_guard<std::mutex> lock(loadListMutex);
        tempLoadList = std::move(m_vpChunkLoadList);
        m_vpChunkLoadList.clear();
    }

    std::vector<Chunk*> chunksToSetup;
    int lNumOfChunksLoaded = 0;

    for (auto pChunk : tempLoadList) {
        if (!pChunk->IsLoaded() && lNumOfChunksLoaded < ASYNC_NUM_CHUNKS_PER_FRAME) {
            pChunk->LoadChunk(terrainGenerator);
            lNumOfChunksLoaded++;
            m_forceVisibilityUpdate = true;
            chunksToSetup.push_back(pChunk);
        }
    }

    if (!chunksToSetup.empty()) {
        std::lock_guard<std::mutex> lock(setupListMutex);
        m_vpChunkSetupList.insert(m_vpChunkSetupList.end(), chunksToSetup.begin(), chunksToSetup.end());
    }
}

void World::UpdateSetupList() {
    std::vector<Chunk*> tempSetupList;
    {
        std::lock_guard<std::mutex> lock(setupListMutex);
        tempSetupList = std::move(m_vpChunkSetupList);
        m_vpChunkSetupList.clear();
    }

    std::vector<Chunk*> chunksToRebuild;

    for (auto pChunk : tempSetupList) {
        if (!pChunk->IsSetup()) {
            pChunk->SetupChunk();
            chunksToRebuild.push_back(pChunk);
            UpdateAdjacentChunks(pChunk);
            m_forceVisibilityUpdate = true;
        }
    }

    if (!chunksToRebuild.empty()) {
        std::lock_guard<std::mutex> lock(rebuildListMutex);
        m_vpChunkRebuildList.insert(m_vpChunkRebuildList.end(), chunksToRebuild.begin(), chunksToRebuild.end());
    }
}

void World::UpdateRebuildList() {
    std::vector<Chunk*> tempRebuildList;
    {
        std::lock_guard<std::mutex> lock(rebuildListMutex);
        tempRebuildList = std::move(m_vpChunkRebuildList);
        m_vpChunkRebuildList.clear();
    }

    std::vector<Chunk*> chunksToUpdateFlags;
    int lNumRebuiltChunkThisFrame = 0;

    for (auto pChunk : tempRebuildList) {
        if (pChunk->IsLoaded() && pChunk->IsSetup() && lNumRebuiltChunkThisFrame < ASYNC_NUM_CHUNKS_PER_FRAME) {
            pChunk->GenerateMesh();
            chunksToUpdateFlags.push_back(pChunk);

            // Add neighbors
            auto coords = pChunk->GetCoords();
            auto pChunkXMinus = GetChunk(coords.x - 1, coords.y, coords.z);
            auto pChunkXPlus = GetChunk(coords.x + 1, coords.y, coords.z);
            auto pChunkYMinus = GetChunk(coords.x, coords.y - 1, coords.z);
            auto pChunkYPlus = GetChunk(coords.x, coords.y + 1, coords.z);
            auto pChunkZMinus = GetChunk(coords.x, coords.y, coords.z - 1);
            auto pChunkZPlus = GetChunk(coords.x, coords.y, coords.z + 1);

            if (pChunkXMinus) chunksToUpdateFlags.push_back(pChunkXMinus);
            if (pChunkXPlus) chunksToUpdateFlags.push_back(pChunkXPlus);
            if (pChunkYMinus) chunksToUpdateFlags.push_back(pChunkYMinus);
            if (pChunkYPlus) chunksToUpdateFlags.push_back(pChunkYPlus);
            if (pChunkZMinus) chunksToUpdateFlags.push_back(pChunkZMinus);
            if (pChunkZPlus) chunksToUpdateFlags.push_back(pChunkZPlus);

            lNumRebuiltChunkThisFrame++;
            m_forceVisibilityUpdate = true;
        }
    }

    if (!chunksToUpdateFlags.empty()) {
        std::lock_guard<std::mutex> lock(flagsListMutex);
        m_vpChunkUpdateFlagsList.insert(m_vpChunkUpdateFlagsList.end(), chunksToUpdateFlags.begin(), chunksToUpdateFlags.end());
    }
}

void World::UpdateFlagsList() {
    std::vector<Chunk*> tempFlagsList;
    {
        std::lock_guard<std::mutex> lock(flagsListMutex);
        tempFlagsList = std::move(m_vpChunkUpdateFlagsList);
        m_vpChunkUpdateFlagsList.clear();
    }

    for (auto pChunk : tempFlagsList) {
        if (pChunk->IsLoaded() && pChunk->IsSetup()) {
            pChunk->UpdateEmptyFullFlags();
        }
    }

    for (auto pChunk : tempFlagsList) {
        if (pChunk->IsLoaded() && pChunk->IsSetup()) {
            int offsets[][3] = {
                { 1, 0, 0 }, {-1, 0, 0 },
                { 0, 1, 0 }, { 0,-1, 0 },
                { 0, 0, 1 }, { 0, 0,-1 },
            };

            bool surrounded = true;
            for (int i = 0; i < 6; i++) {
                auto coords = pChunk->GetCoords();
                auto neighbour = GetChunk(coords.x + offsets[i][0], coords.y + offsets[i][1], coords.z + offsets[i][2]);
                if (neighbour == nullptr || !neighbour->IsFull()) {
                    surrounded = false;
                    break;
                }
            }
            pChunk->SetIsSurrounded(surrounded);
        }
    }
}

void World::UpdateVisibilityList() {
    std::lock_guard<std::mutex> chunkLock(chunksMutex);
    m_vpChunkVisibilityList.clear();
    for (auto iterator = chunks.begin(); iterator != chunks.end(); ++iterator) {
        Chunk* pChunk = (*iterator).second.get();
        if (pChunk->IsLoaded()) {
            if (pChunk->IsSetup()) {
                if (!pChunk->IsEmpty() && !pChunk->IsSurrounded() || !pChunk->IsFull()) {
                    m_vpChunkVisibilityList.push_back(pChunk);
                }
            }
        }
    }

    //std::cout << "Visibility list size: " << m_vpChunkVisibilityList.size() << std::endl;
}

void World::UpdateRenderList() {
    // Clear the render list each frame BEFORE we do our tests to see what chunks should be rendered     
    m_vpChunkRenderList.clear();
    std::lock_guard<std::mutex> chunkLock(chunksMutex);
    for (auto iterator = m_vpChunkVisibilityList.begin(); iterator != m_vpChunkVisibilityList.end(); ++iterator) {
        Chunk* pChunk = *iterator;
        if (pChunk->IsLoaded() && pChunk->IsSetup()) {
            if (pChunk->ShouldRender()) // Early flags check so we don't always have to do the frustum check... 
            {
                /*float c_offset = (Chunk::CHUNK_SIZE - 1) * 0.5f;
                glm::vec3 chunkCenter = glm::vec3(pChunk->GetCoords()) + glm::vec3(c_offset, c_offset, c_offset);
                float c_size = Chunk::CHUNK_SIZE * 0.5f;

                if (m_pRenderer->CubeInFrustum(m_pRenderer->GetActiveViewPort(), chunkCenter, c_size, c_size, c_size)) {
                    m_vpChunkRenderList.push_back(pChunk);
                }*/

                m_vpChunkRenderList.push_back(pChunk);
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
        Chunk* pChunk = *iterator;
        
        auto coords = pChunk->GetCoords();

        glm::mat4 newViewMatrix = glm::translate(viewMatrix, glm::vec3(coords.x * Chunk::CHUNK_SIZE, coords.y * Chunk::CHUNK_SIZE, coords.z * Chunk::CHUNK_SIZE));
        shader.SetUniform("view", newViewMatrix);

        pChunk->Render(shader);
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

void World::QueueMeshGeneration(Chunk* chunk) {
    if (!chunk || chunk->IsGeneratingMesh()) return;

    std::lock_guard<std::mutex> lock(meshQueueMutex);
    meshGenerationQueue.push_back(chunk);
}

void World::ProcessMeshQueue() {
    std::vector<Chunk*> chunksToProcess;
    {
        std::lock_guard<std::mutex> lock(meshQueueMutex);
        if (meshGenerationQueue.empty()) return;

        chunksToProcess = std::move(meshGenerationQueue);
        meshGenerationQueue.clear();
    }

    // Process chunks in parallel batches
    const int batchSize = MESH_GENERATION_THREADS;
    for (size_t i = 0; i < chunksToProcess.size(); i += batchSize) {
        std::vector<std::future<void>> futures;

        for (size_t j = i; j < std::min(i + batchSize, chunksToProcess.size()); ++j) {
            Chunk* chunk = chunksToProcess[j];
            if (chunk && chunk->IsLoaded() && chunk->IsSetup() && !chunk->IsGeneratingMesh()) {
                futures.push_back(std::async(std::launch::async, [chunk]() {
                    chunk->GenerateMesh();
                    }));
            }
        }

        // Wait for batch to complete
        for (auto& future : futures) {
            future.wait();
        }
    }
}

void World::DebugFixChunk(glm::vec3 position) {
    auto chunkCoords = WorldToChunkCoordinates(position);
    DebugFixChunk(chunkCoords.x, chunkCoords.y, chunkCoords.z);
}

void World::DebugFixChunk(int chunkX, int chunkY, int chunkZ) {
    auto pChunk = GetChunk(chunkX, chunkY, chunkZ);
    if (pChunk) {
        std::cout << "=== Debugging chunk (" << chunkX << "," << chunkY << "," << chunkZ << ") ===" << std::endl;
        pChunk->DebugPrintState();

        // Force regenerate mesh
        pChunk->GenerateMesh();

        // Force update flags
        pChunk->UpdateEmptyFullFlags();

        // Force visibility update
        m_forceVisibilityUpdate = true;

        std::cout << "After fix:" << std::endl;
        pChunk->DebugPrintState();
        std::cout << "=== End debug ===" << std::endl;
    }
}