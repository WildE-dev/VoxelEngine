#include "world/Chunk.h"

#include <algorithm>
#include <cmath>

#include "world/TerrainGenerator.h"

std::atomic<int> Chunk::liveCount{0};

Chunk::Chunk(glm::ivec3 coords) : coords(coords) {
    liveCount++;
}

Chunk::~Chunk() {
    // GL objects must be released on the GL thread via ReleaseGpu() before the
    // chunk is dropped; the destructor may run on the worker thread.
    liveCount--;
}

Block Chunk::GetBlock(int x, int y, int z) const {
    std::lock_guard<std::mutex> lock(blockMutex);
    return blocks[Index(x, y, z)];
}

bool Chunk::IsOpaque(int x, int y, int z) const {
    std::lock_guard<std::mutex> lock(blockMutex);
    return blocks[Index(x, y, z)].IsFullBlock();
}

void Chunk::SetBlock(int x, int y, int z, const Block& block) {
    std::lock_guard<std::mutex> lock(blockMutex);
    blocks[Index(x, y, z)] = block;
}

bool Chunk::TryBeginGenerate() {
    State expected = State::Pending;
    return state.compare_exchange_strong(expected, State::Generating);
}

void Chunk::Generate(const TerrainGenerator& terrain) {
    // Build into a local buffer so the lock is only held for the final copy.
    auto generated = std::make_unique<std::array<Block, VOLUME>>();
    auto& out = *generated;

    float heights[CHUNK_SIZE + 1][CHUNK_SIZE + 1];
    for (int x = 0; x <= CHUNK_SIZE; x++) {
        for (int z = 0; z <= CHUNK_SIZE; z++) {
            heights[x][z] = terrain.GetHeight(coords.x * CHUNK_SIZE + x, coords.z * CHUNK_SIZE + z);
        }
    }

    const float smoothness = 8.0f;
    auto quantise = [&](float h) { return std::round(h * smoothness) / smoothness; };
    // Height above the block's base in eighths, clamped to the valid edge range.
    auto edgeHeight = [](float h) { return static_cast<uint8_t>(std::clamp(static_cast<int>(h * 8), 0, 8)); };

    for (int x = 0; x < CHUNK_SIZE; x++) {
        for (int z = 0; z < CHUNK_SIZE; z++) {
            // Corner heights, in edge order used by EdgeData.
            float h0 = quantise(heights[x][z + 1]);
            float h1 = quantise(heights[x + 1][z]);
            float h2 = quantise(heights[x][z]);
            float h3 = quantise(heights[x + 1][z + 1]);

            int minHeight = static_cast<int>(std::floor(std::min(std::min(h0, h1), std::min(h2, h3))));

            for (int y = 0; y < CHUNK_SIZE; y++) {
                int blockY = coords.y * CHUNK_SIZE + y;

                if (blockY == minHeight) {
                    EdgeData edges;
                    edges.SetTopY(0, edgeHeight(h0 - blockY));
                    edges.SetTopY(1, edgeHeight(h1 - blockY));
                    edges.SetTopY(2, edgeHeight(h2 - blockY));
                    edges.SetTopY(3, edgeHeight(h3 - blockY));

                    if (edges.IsValid()) {
                        out[Index(x, y, z)] = Block(BlockType::GRASS, edges);
                    }
                    else if (y > 0) {
                        // Surface is (almost) flat at the bottom of this cell: put a full
                        // grass block underneath instead. At y == 0 that block belongs to
                        // the chunk below, which generates dirt there.
                        out[Index(x, y - 1, z)] = Block(BlockType::GRASS);
                    }
                }
                else if (blockY < minHeight && blockY > minHeight - 5) {
                    out[Index(x, y, z)] = Block(BlockType::DIRT);
                }
                else if (blockY < minHeight) {
                    out[Index(x, y, z)] = Block(BlockType::STONE);
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(blockMutex);
        blocks = out;
    }
    state = State::Loaded;
}

void Chunk::BuildMesh(const Neighbours& neighbours) {
    // Snapshot our own blocks so edits from the main thread can't tear the mesh.
    auto snapshot = std::make_unique<std::array<Block, VOLUME>>();
    {
        std::lock_guard<std::mutex> lock(blockMutex);
        *snapshot = blocks;
    }
    const auto& local = *snapshot;

    std::vector<uint32_t> vertices;

    for (int x = 0; x < CHUNK_SIZE; ++x) {
        for (int y = 0; y < CHUNK_SIZE; ++y) {
            for (int z = 0; z < CHUNK_SIZE; ++z) {
                const Block& block = local[Index(x, y, z)];
                if (block.IsAir()) continue;

                const bool full = block.IsFullBlock();

                for (int face = 0; face < kFaceCount; ++face) {
                    int nx = x + kFaceOffsets[face][0];
                    int ny = y + kFaceOffsets[face][1];
                    int nz = z + kFaceOffsets[face][2];

                    bool neighbourCulls;
                    if (nx >= 0 && nx < CHUNK_SIZE && ny >= 0 && ny < CHUNK_SIZE && nz >= 0 && nz < CHUNK_SIZE) {
                        neighbourCulls = local[Index(nx, ny, nz)].IsFullBlock();
                    }
                    else {
                        // Only one axis can be out of range for a face neighbour; wrap it
                        // into the adjacent chunk. Unloaded neighbours never cull, and
                        // they'll trigger a rebuild of this chunk once they load.
                        const auto& neighbour = neighbours[face];
                        neighbourCulls = neighbour && neighbour->IsLoaded() &&
                            neighbour->IsOpaque((nx + CHUNK_SIZE) % CHUNK_SIZE,
                                                (ny + CHUNK_SIZE) % CHUNK_SIZE,
                                                (nz + CHUNK_SIZE) % CHUNK_SIZE);
                    }

                    if (!neighbourCulls || !full) {
                        block.AddFaceVertices(vertices, static_cast<Face>(face), x, y, z);
                    }
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(meshMutex);
    pendingMesh = std::move(vertices);
    hasPendingMesh = true;
}

void Chunk::UploadPendingMesh() {
    if (removed) return;

    {
        std::lock_guard<std::mutex> lock(meshMutex);
        if (hasPendingMesh) {
            if (!vao) {
                glGenVertexArrays(1, &vao);
                glGenBuffers(1, &vbo);
                glBindVertexArray(vao);
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, 2 * sizeof(uint32_t), (void*)0);
                glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, 2 * sizeof(uint32_t), (void*)sizeof(uint32_t));
                glEnableVertexAttribArray(0);
                glEnableVertexAttribArray(1);
            }

            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, pendingMesh.size() * sizeof(uint32_t), pendingMesh.data(), GL_STATIC_DRAW);
            vertexCount = static_cast<GLsizei>(pendingMesh.size() / 2);

            std::vector<uint32_t>().swap(pendingMesh);
            hasPendingMesh = false;
        }
    }
}

void Chunk::Draw() const {
    if (vertexCount == 0) return;

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);
}

void Chunk::ReleaseGpu() {
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    vao = vbo = 0;
    vertexCount = 0;
}
