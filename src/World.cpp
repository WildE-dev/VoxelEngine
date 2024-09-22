#include <iostream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "World.h"
#include "Shader.h"

inline static int rem(int a, int b) {
    return ((a % b) + b) % b;
}

World::World(TerrainGenerator* terrainGenerator) : terrainGenerator(terrainGenerator) {}

World::World(const World& other) : terrainGenerator(other.terrainGenerator) {}

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

Chunk* World::GetChunk(int chunkX, int chunkY, int chunkZ)
{
    Chunk* ptr = nullptr;

    auto key = std::make_tuple(chunkX, chunkY, chunkZ);
    if (chunks.find(key) != chunks.end()) {
        ptr = chunks[key].get();
    }

    return ptr;
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

    std::vector<glm::ivec3> chunksToRebuild;

    if (blockCoords.x == 0) chunksToRebuild.push_back(WorldToChunkCoordinates(x - 1, y, z));
    if (blockCoords.x == Chunk::CHUNK_SIZE - 1) chunksToRebuild.push_back(WorldToChunkCoordinates(x + 1, y, z));
    if (blockCoords.y == 0) chunksToRebuild.push_back(WorldToChunkCoordinates(x, y - 1, z));
    if (blockCoords.y == Chunk::CHUNK_SIZE - 1) chunksToRebuild.push_back(WorldToChunkCoordinates(x, y + 1, z));
    if (blockCoords.z == 0) chunksToRebuild.push_back(WorldToChunkCoordinates(x, y, z - 1));
    if (blockCoords.z == Chunk::CHUNK_SIZE - 1) chunksToRebuild.push_back(WorldToChunkCoordinates(x, y, z + 1));

    for (int i = 0; i < chunksToRebuild.size(); i++)
    {
        auto chunkCoords = chunksToRebuild[i];
        auto pChunk = GetChunk(chunkCoords.x, chunkCoords.y, chunkCoords.z);
        if (pChunk != NULL) {
            pChunk->SetNeedsRebuilding(true);
        }
    }
}

void World::UpdateAdjacentChunks(Chunk* chunk) {
    auto coords = chunk->GetCoords();

    std::vector<glm::ivec3> chunksToRebuild;

    chunksToRebuild.push_back(glm::ivec3(coords.x - 1, coords.y, coords.z));
    chunksToRebuild.push_back(glm::ivec3(coords.x + 1, coords.y, coords.z));
    chunksToRebuild.push_back(glm::ivec3(coords.x, coords.y - 1, coords.z));
    chunksToRebuild.push_back(glm::ivec3(coords.x, coords.y + 1, coords.z));
    chunksToRebuild.push_back(glm::ivec3(coords.x, coords.y, coords.z - 1));
    chunksToRebuild.push_back(glm::ivec3(coords.x, coords.y, coords.z + 1));

    for (int i = 0; i < chunksToRebuild.size(); i++)
    {
        auto chunkCoords = chunksToRebuild[i];
        auto pChunk = GetChunk(chunkCoords.x, chunkCoords.y, chunkCoords.z);
        if (pChunk) {
            pChunk->SetNeedsRebuilding(true);
        }
    }
}

void World::Update(glm::vec3 cameraPosition, glm::vec3 cameraView, ThreadPool& threadPool) {
    UpdateAsyncChunker(cameraPosition);
    UpdateLoadList(threadPool);
    UpdateSetupList();
    UpdateRebuildList();
    UpdateFlagsList();
    UpdateUnloadList();
    UpdateVisibilityList(cameraPosition);
    if (m_cameraPosition != cameraPosition || m_cameraView != cameraView || m_forceVisibilityUpdate) {
        UpdateRenderList();
    }
    m_cameraPosition = cameraPosition;
    m_cameraView = cameraView;
}

void World::UpdateAsyncChunker(glm::vec3 position) {
    auto chunkCoords = WorldToChunkCoordinates(position);

    for (auto iterator = chunks.begin(); iterator != chunks.end(); ++iterator) {
        Chunk* pChunk = (*iterator).second.get();
        auto coords = pChunk->GetCoords();

        int distX = abs((coords.x - chunkCoords.x));
        int distY = abs((coords.y - chunkCoords.y));
        int distZ = abs((coords.z - chunkCoords.z));


        if (distX > RENDER_DISTANCE + 1 || distY > RENDER_DISTANCE + 1 || distZ > RENDER_DISTANCE + 1) {
            m_vpChunkUnloadList.push_back(pChunk);
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
                    chunks[key] = std::unique_ptr<Chunk>(new Chunk(this, chunkX, chunkY, chunkZ));
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

void World::UpdateLoadList(ThreadPool& threadPool) {
    int lNumOfChunksLoaded = 0;
    for (auto iterator = m_vpChunkLoadList.begin(); iterator != m_vpChunkLoadList.end() && (lNumOfChunksLoaded < ASYNC_NUM_CHUNKS_PER_FRAME); ++iterator) {
        Chunk* pChunk = *iterator;
        if (!pChunk->IsLoaded()) {
            threadPool.QueueJob([this, pChunk] { pChunk->LoadChunk(terrainGenerator); });
            lNumOfChunksLoaded++;
            m_forceVisibilityUpdate = true;

            m_vpChunkSetupList.push_back(pChunk);
        }
    }   
    m_vpChunkLoadList.clear();
}

void World::UpdateSetupList() {
    for (auto iterator = m_vpChunkSetupList.begin(); iterator != m_vpChunkSetupList.end(); ++iterator) {
        Chunk* pChunk = *iterator;
        if (!pChunk->IsSetup()) {
            pChunk->SetupChunk();
            m_vpChunkRebuildList.push_back(pChunk);
            UpdateAdjacentChunks(pChunk);
            m_forceVisibilityUpdate = true;
        }
    }
    m_vpChunkSetupList.clear();
}

void World::UpdateRebuildList() {
    // Rebuild any chunks that are in the rebuild chunk list     
    int lNumRebuiltChunkThisFrame = 0;
    for (auto iterator = m_vpChunkRebuildList.begin(); iterator != m_vpChunkRebuildList.end() && (lNumRebuiltChunkThisFrame < ASYNC_NUM_CHUNKS_PER_FRAME); ++iterator) {
        Chunk* pChunk = *iterator;
        if (pChunk->IsLoaded() && pChunk->IsSetup()) {
            pChunk->GenerateMesh();
            // If we rebuild a chunk, add it to the list of chunks that need their render flags updated                 
            // since we might now be empty or surrounded
            m_vpChunkUpdateFlagsList.push_back(pChunk);

            // Also add our neighbours since they might now be surrounded too (If we have neighbours)         
            auto coords = pChunk->GetCoords();
            auto pChunkXMinus = GetChunk(coords.x - 1, coords.y, coords.z);
            auto pChunkXPlus = GetChunk(coords.x + 1, coords.y, coords.z);
            auto pChunkYMinus = GetChunk(coords.x, coords.y - 1, coords.z);
            auto pChunkYPlus = GetChunk(coords.x, coords.y + 1, coords.z);
            auto pChunkZMinus = GetChunk(coords.x, coords.y, coords.z - 1);
            auto pChunkZPlus = GetChunk(coords.x, coords.y, coords.z + 1);
            if (pChunkXMinus != NULL) m_vpChunkUpdateFlagsList.push_back(pChunkXMinus);
            if (pChunkXPlus != NULL) m_vpChunkUpdateFlagsList.push_back(pChunkXPlus);
            if (pChunkYMinus != NULL) m_vpChunkUpdateFlagsList.push_back(pChunkYMinus);
            if (pChunkYPlus != NULL) m_vpChunkUpdateFlagsList.push_back(pChunkYPlus);
            if (pChunkZMinus != NULL) m_vpChunkUpdateFlagsList.push_back(pChunkZMinus);
            if (pChunkZPlus != NULL) m_vpChunkUpdateFlagsList.push_back(pChunkZPlus);

            // Only rebuild a certain number of chunks per frame
            lNumRebuiltChunkThisFrame++;
            m_forceVisibilityUpdate = true;
        }
    }
    // Clear the rebuild list     
    m_vpChunkRebuildList.clear();
}

void World::UpdateFlagsList() {
    for (auto iterator = m_vpChunkUpdateFlagsList.begin(); iterator != m_vpChunkUpdateFlagsList.end(); ++iterator) {
        Chunk* pChunk = *iterator;
        if (pChunk->IsLoaded() && pChunk->IsSetup()) {
            pChunk->UpdateEmptyFullFlags();
        }
    }
    for (auto iterator = m_vpChunkUpdateFlagsList.begin(); iterator != m_vpChunkUpdateFlagsList.end(); ++iterator) {
        Chunk* pChunk = *iterator;
        if (pChunk->IsLoaded() && pChunk->IsSetup()) {
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

            pChunk->SetIsSurrounded(surrounded);
        }
    }
    m_vpChunkUpdateFlagsList.clear();
}

void World::UpdateUnloadList() {
    //std::cout << "Unload list size: " << m_vpChunkUnloadList.size() << std::endl;
    int lNumUnloadedChunkThisFrame = 0;
    for (auto iterator = m_vpChunkUnloadList.begin(); iterator != m_vpChunkUnloadList.end() && (lNumUnloadedChunkThisFrame < ASYNC_NUM_CHUNKS_PER_FRAME); ++iterator) {
        Chunk* pChunk = *iterator;
        if (pChunk->IsLoaded()) {
            pChunk->UnloadChunk();
            auto coords = pChunk->GetCoords();
            chunks.erase(std::make_tuple(coords.x, coords.y, coords.z));
            lNumUnloadedChunkThisFrame++;
            m_forceVisibilityUpdate = true;
        }
    } 
    m_vpChunkUnloadList.clear();
}

void World::UpdateVisibilityList(glm::vec3 cameraPosition) {
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

void World::Render(Shader& shader, glm::mat4& viewMatrix, glm::mat4& projectionMatrix, float frameWidth, float frameHeight) {
    shader.Use();

    glm::mat4 model = glm::mat4(1.0f);

    shader.SetUniform("model", model);
    shader.SetUniform("projection", projectionMatrix);

    /*for (auto& pair : chunks) {
        if (!pair.second.get()->GetIsGenerated()) {
            continue;
        }

        int chunkX = std::get<0>(pair.first);
        int chunkY = std::get<1>(pair.first);
        int chunkZ = std::get<2>(pair.first);

        glm::mat4 newViewMatrix = glm::translate(viewMatrix, glm::vec3(chunkX * Chunk::CHUNK_SIZE, chunkY * Chunk::CHUNK_SIZE, chunkZ * Chunk::CHUNK_SIZE));
        shader.SetUniform("view", newViewMatrix);

        pair.second.get()->Render(shader);
    }*/

    for (auto iterator = m_vpChunkRenderList.begin(); iterator != m_vpChunkRenderList.end(); ++iterator) {
        Chunk* pChunk = *iterator;
        
        auto coords = pChunk->GetCoords();

        glm::mat4 newViewMatrix = glm::translate(viewMatrix, glm::vec3(coords.x * Chunk::CHUNK_SIZE, coords.y * Chunk::CHUNK_SIZE, coords.z * Chunk::CHUNK_SIZE));
        shader.SetUniform("view", newViewMatrix);

        pChunk->Render(shader);
    }
}

void World::RebuildAllChunks()
{
    for (auto iterator = chunks.begin(); iterator != chunks.end(); ++iterator) {
        Chunk* pChunk = (*iterator).second.get();
        pChunk->SetNeedsRebuilding(true);
    }
}