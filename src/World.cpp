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

    if (!chunks[key].get()->GetIsGenerated()) {
        return false;
    }

    chunk = chunks[key].get();
    return true;
}

std::shared_ptr<Chunk> World::GetChunk(int chunkX, int chunkY, int chunkZ)
{
    std::shared_ptr<Chunk> ptr;

    auto key = std::make_tuple(chunkX, chunkY, chunkZ);
    if (chunks.find(key) != chunks.end()) {
        ptr = chunks[key];
    }

    return ptr;
}

Block World::GetBlock(int x, int y, int z) {
    auto chunkCoords = WorldToChunkCoordinates(x, y, z);
    auto blockCoords = WorldToBlockCoordinates(x, y, z);
    Chunk* chunk = nullptr;
    if (GetChunk(chunk, chunkCoords.x, chunkCoords.y, chunkCoords.z)) {
        return chunk->GetBlock(blockCoords.x, blockCoords.y, blockCoords.z);
    }
    return Block();
}

bool World::GetBlockCulls(int x, int y, int z) {
    auto chunkCoords = WorldToChunkCoordinates(x, y, z);
    auto blockCoords = WorldToBlockCoordinates(x, y, z);
    Chunk* chunk = nullptr;
    if (GetChunk(chunk, chunkCoords.x, chunkCoords.y, chunkCoords.z)) {
        return chunk->GetBlockCulls(blockCoords.x, blockCoords.y, blockCoords.z);
    }
    return false;
}

void World::SetBlock(int x, int y, int z, Block block)
{
    auto chunkCoords = WorldToChunkCoordinates(x, y, z);
    auto blockCoords = WorldToBlockCoordinates(x, y, z);
    Chunk* chunk = nullptr;
    if (GetChunk(chunk, chunkCoords.x, chunkCoords.y, chunkCoords.z)) {
        chunk->SetBlock(blockCoords.x, blockCoords.y, blockCoords.z, block);
        UpdateAdjacentChunks(x, y, z);
    }
}

void World::SetBlock(int x, int y, int z, BlockType type) {
    auto chunkCoords = WorldToChunkCoordinates(x, y, z);
    auto blockCoords = WorldToBlockCoordinates(x, y, z);
    Chunk* chunk = nullptr;
    if (GetChunk(chunk, chunkCoords.x, chunkCoords.y, chunkCoords.z)) {
        chunk->SetBlock(blockCoords.x, blockCoords.y, blockCoords.z, type);
        UpdateAdjacentChunks(x, y, z);
    }
}

void World::SetBlock(int x, int y, int z, EdgeData edges) {
    auto chunkCoords = WorldToChunkCoordinates(x, y, z);
    auto blockCoords = WorldToBlockCoordinates(x, y, z);
    Chunk* chunk = nullptr;
    if (GetChunk(chunk, chunkCoords.x, chunkCoords.y, chunkCoords.z)) {
        chunk->SetBlock(blockCoords.x, blockCoords.y, blockCoords.z, edges);
        UpdateAdjacentChunks(x, y, z);
    }
}

void World::UpdateAdjacentChunks(int x, int y, int z) {
    Chunk* chunk = nullptr;
    glm::ivec3 chunkCoords;
    glm::ivec3 blockCoords = WorldToBlockCoordinates(x, y, z);

    // Check -X boundary
    if (blockCoords.x == 0) {
        chunkCoords = WorldToChunkCoordinates(x - 1, y, z);
        if (GetChunk(chunk, chunkCoords.x, chunkCoords.y, chunkCoords.z)) {
            //chunk->GenerateMesh(*this);
            chunk->SetIsGenerated(false);
        }
    }
    // Check +X boundary
    if (blockCoords.x == Chunk::CHUNK_SIZE - 1) {
        chunkCoords = WorldToChunkCoordinates(x + 1, y, z);
        if (GetChunk(chunk, chunkCoords.x, chunkCoords.y, chunkCoords.z)) {
            //chunk->GenerateMesh(*this);
            chunk->SetIsGenerated(false);
        }
    }

    // Check -Y boundary
    if (blockCoords.y == 0) {
        chunkCoords = WorldToChunkCoordinates(x, y - 1, z);
        if (GetChunk(chunk, chunkCoords.x, chunkCoords.y, chunkCoords.z)) {
            //chunk->GenerateMesh(*this);
            chunk->SetIsGenerated(false);
        }
    }
    // Check +Y boundary
    if (blockCoords.y == Chunk::CHUNK_SIZE - 1) {
        chunkCoords = WorldToChunkCoordinates(x, y + 1, z);
        if (GetChunk(chunk, chunkCoords.x, chunkCoords.y, chunkCoords.z)) {
            //chunk->GenerateMesh(*this);
            chunk->SetIsGenerated(false);
        }
    }

    // Check -Z boundary
    if (blockCoords.z == 0) {
        chunkCoords = WorldToChunkCoordinates(x, y, z - 1);
        if (GetChunk(chunk, chunkCoords.x, chunkCoords.y, chunkCoords.z)) {
            //chunk->GenerateMesh(*this);
            chunk->SetIsGenerated(false);
        }
    }
    // Check +Z boundary
    if (blockCoords.z == Chunk::CHUNK_SIZE - 1) {
        chunkCoords = WorldToChunkCoordinates(x, y, z + 1);
        if (GetChunk(chunk, chunkCoords.x, chunkCoords.y, chunkCoords.z)) {
            //chunk->GenerateMesh(*this);
            chunk->SetIsGenerated(false);
        }
    }
}

void World::MarkAdjacentChunks(glm::ivec3 chunkCoords) {
    Chunk* chunk = nullptr;

    const int offsets[6][3] = {
        { 1, 0, 0 },
        { -1, 0, 0 },
        { 0, 1, 0 },
        { 0, -1, 0 },
        { 0, 0, 1 },
        { 0, 0, -1 },
    };

    for (size_t i = 0; i < 6; i++)
    {
        int x = chunkCoords.x + offsets[i][0];
        int y = chunkCoords.y + offsets[i][1];
        int z = chunkCoords.z + offsets[i][2];

        if (GetChunk(chunk, x, y, z)) {
            chunk->SetIsGenerated(false);
        }
    }
}

void World::QueueChunkLoad(std::shared_ptr<Chunk> chunk) {
    //chunkQueue.Add(chunk);
}

void World::LoadChunks() {
    //auto chunk = chunkQueue.Get();
    
    //if (chunk != NULL)
        //chunk->LoadChunk();
}

void World::Update(glm::vec3 cameraPosition, glm::vec3 cameraView) {
    UpdateAsyncChunker(cameraPosition);
    UpdateLoadList();
    UpdateSetupList();
    UpdateRebuildList();
    UpdateFlagsList();
    UpdateUnloadList();
    UpdateVisibilityList(cameraPosition);
    if (m_cameraPosition != cameraPosition || m_cameraView != cameraView) {
        UpdateRenderList();
    }
    m_cameraPosition = cameraPosition;
    m_cameraView = cameraView;
}

void World::UpdateLoadList() {
    int lNumOfChunksLoaded = 0;
    for (auto iterator = m_vpChunkLoadList.begin(); iterator != m_vpChunkLoadList.end() && (lNumOfChunksLoaded < ASYNC_NUM_CHUNKS_PER_FRAME); ++iterator) {
        std::shared_ptr<Chunk> pChunk = *iterator;
        if (!pChunk->IsLoaded()) {
            pChunk->LoadChunk();
            lNumOfChunksLoaded++;
            //m_forceVisibilityUpdate = true;

            m_vpChunkSetupList.push_back(pChunk);
        }
    }   
    m_vpChunkLoadList.clear();
}

void World::UpdateSetupList() {
    for (auto iterator = m_vpChunkSetupList.begin(); iterator != m_vpChunkSetupList.end(); ++iterator) {
        std::shared_ptr<Chunk> pChunk = *iterator;
        if (!pChunk->IsSetup()) {
            pChunk->SetupChunk();
            //m_forceVisibilityUpdate = true;
        }
    }
    m_vpChunkSetupList.clear();
}

void World::UpdateRebuildList() {
    // Rebuild any chunks that are in the rebuild chunk list     
    int lNumRebuiltChunkThisFrame = 0;
    for (auto iterator = m_vpChunkRebuildList.begin(); iterator != m_vpChunkRebuildList.end() && (lNumRebuiltChunkThisFrame != ASYNC_NUM_CHUNKS_PER_FRAME); ++iterator) {
        std::shared_ptr<Chunk> pChunk = *iterator;
        if (pChunk->IsLoaded() && pChunk->IsSetup()) {
            if (lNumRebuiltChunkThisFrame != ASYNC_NUM_CHUNKS_PER_FRAME) {
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
                //m_forceVisibilityUpdate = true;
            }
        }
    }
    // Clear the rebuild list     
    m_vpChunkRebuildList.clear();
}

void World::UpdateFlagsList() {
    for (auto iterator = m_vpChunkUpdateFlagsList.begin(); iterator != m_vpChunkUpdateFlagsList.end(); ++iterator) {
        std::shared_ptr<Chunk> pChunk = *iterator;
        // TODO: implement this
    }
    m_vpChunkUpdateFlagsList.clear();
}

void World::UpdateUnloadList() {
    for (auto iterator = m_vpChunkUnloadList.begin(); iterator != m_vpChunkUnloadList.end(); ++iterator) {
        std::shared_ptr<Chunk> pChunk = *iterator;
        if (pChunk->IsLoaded()) {
            pChunk->UnloadChunk();
            auto coords = pChunk->GetCoords();
            chunks.erase(std::make_tuple(coords.x, coords.y, coords.z));
            //m_forceVisibilityUpdate = true;
        }
    } 
    m_vpChunkUnloadList.clear();
}

void World::UpdateVisibilityList(glm::vec3 cameraPosition) {
    m_vpChunkVisibilityList.clear();
    for (auto iterator = chunks.begin(); iterator != chunks.end(); ++iterator) {
        std::shared_ptr<Chunk> pChunk = (*iterator).second;
        if (pChunk->IsLoaded()) {
            if (pChunk->IsSetup()) {
                // TODO: Check if is surrounded/empty
                m_vpChunkVisibilityList.push_back(pChunk);
            }
        }
    }
}

void World::UpdateRenderList() {
    // Clear the render list each frame BEFORE we do our tests to see what chunks should be rendered     
    m_vpChunkRenderList.clear();
    for (auto iterator = m_vpChunkVisibilityList.begin(); iterator != m_vpChunkVisibilityList.end(); ++iterator) {
        std::shared_ptr<Chunk> pChunk = *iterator;
        if (pChunk != NULL) {
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
    }
}

void World::UpdateAsyncChunker(glm::vec3 position) {
    auto chunkCoords = WorldToChunkCoordinates(position);

    for (auto iterator = chunks.begin(); iterator != chunks.end(); ++iterator) {
        std::shared_ptr<Chunk> pChunk = (*iterator).second;
        auto coords = pChunk->GetCoords();

        int distX = abs((coords.x - chunkCoords.x));
        int distY = abs((coords.y - chunkCoords.y));
        int distZ = abs((coords.z - chunkCoords.z));
        

        if (distX > UNLOAD_DISTANCE || distY > UNLOAD_DISTANCE || distZ > UNLOAD_DISTANCE) {
            m_vpChunkUnloadList.push_back(pChunk);
        }
    }

    for (int x = -RENDER_DISTANCE; x < RENDER_DISTANCE; ++x) {
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
            }
        }
    }
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
        std::shared_ptr<Chunk> pChunk = *iterator;
        
        auto coords = pChunk->GetCoords();

        glm::mat4 newViewMatrix = glm::translate(viewMatrix, glm::vec3(coords.x * Chunk::CHUNK_SIZE, coords.y * Chunk::CHUNK_SIZE, coords.z * Chunk::CHUNK_SIZE));
        shader.SetUniform("view", newViewMatrix);

        pChunk->Render(shader);
    }
}