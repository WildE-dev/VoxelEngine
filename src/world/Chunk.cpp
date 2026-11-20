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

void Chunk::CopyBlocks(std::array<Block, VOLUME>& out) const {
    std::lock_guard<std::mutex> lock(blockMutex);
    out = blocks;
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

void Chunk::ComputeColumnHeights(const TerrainGenerator& terrain, int chunkX, int chunkZ, ColumnHeights& out) {
    for (int x = 0; x <= CHUNK_SIZE; x++)
        for (int z = 0; z <= CHUNK_SIZE; z++)
            out[x * (CHUNK_SIZE + 1) + z] = terrain.GetHeight(chunkX * CHUNK_SIZE + x, chunkZ * CHUNK_SIZE + z);
}

void Chunk::Generate(const ColumnHeights& columnHeights) {
    // Build into a local buffer so the lock is only held for the final copy.
    auto generated = std::make_unique<std::array<Block, VOLUME>>();
    auto& out = *generated;

    auto heightAt = [&](int x, int z) { return columnHeights[x * (CHUNK_SIZE + 1) + z]; };

    const float smoothness = 8.0f;
    auto quantise = [&](float h) { return std::round(h * smoothness) / smoothness; };
    // Height above the block's base in eighths, clamped to the valid edge range.
    auto edgeHeight = [](float h) { return static_cast<uint8_t>(std::clamp(static_cast<int>(h * 8), 0, 8)); };

    for (int x = 0; x < CHUNK_SIZE; x++) {
        for (int z = 0; z < CHUNK_SIZE; z++) {
            // Corner heights, in edge order used by EdgeData.
            float h0 = quantise(heightAt(x, z + 1));
            float h1 = quantise(heightAt(x + 1, z));
            float h2 = quantise(heightAt(x, z));
            float h3 = quantise(heightAt(x + 1, z + 1));

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

    // Fast path: an all-air chunk has no faces, whatever its neighbours are.
    // Roughly half of a freshly loaded world is open sky.
    if (std::all_of(local.begin(), local.end(), [](const Block& b) { return b.IsAir(); })) {
        std::lock_guard<std::mutex> lock(meshMutex);
        pendingMesh.clear();
        hasPendingMesh = true;
        return;
    }

    // Opacity of this chunk plus a one block border taken from the surrounding
    // chunks, addressed with coordinates in [-1, CHUNK_SIZE]. Unloaded neighbours
    // count as transparent; they trigger a rebuild of this chunk once they load.
    constexpr int P = CHUNK_SIZE + 2;
    auto paddedIndex = [](int x, int y, int z) { return (x + 1) + P * ((y + 1) + P * (z + 1)); };
    auto opaque = std::make_unique<std::array<bool, P * P * P>>();
    auto& grid = *opaque;
    // Packed block + sky light over the same padded region. Light is worker-only,
    // so neighbours' light can be read without locking.
    auto lightGrid = std::make_unique<std::array<uint8_t, P * P * P>>();
    auto& lights = *lightGrid;
    grid.fill(false);
    lights.fill(0);

    for (int z = 0; z < CHUNK_SIZE; z++)
        for (int y = 0; y < CHUNK_SIZE; y++)
            for (int x = 0; x < CHUNK_SIZE; x++) {
                grid[paddedIndex(x, y, z)] = local[Index(x, y, z)].IsFullBlock();
                lights[paddedIndex(x, y, z)] = light[Index(x, y, z)];
            }

    // Border cells, one neighbour at a time so each neighbour is locked only once.
    // Along each axis a neighbour covers either the single layer beyond our edge
    // (offset -1 or +1) or our whole extent (offset 0).
    auto span = [](int d, int& lo, int& hi) {
        lo = d < 0 ? -1 : d > 0 ? CHUNK_SIZE : 0;
        hi = d < 0 ? -1 : d > 0 ? CHUNK_SIZE : CHUNK_SIZE - 1;
    };
    for (int dz = -1; dz <= 1; dz++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                const auto& neighbour = neighbours[NeighbourIndex(dx, dy, dz)];
                if (!neighbour || !neighbour->IsLoaded()) continue;

                int x0, x1, y0, y1, z0, z1;
                span(dx, x0, x1);
                span(dy, y0, y1);
                span(dz, z0, z1);

                std::lock_guard<std::mutex> lock(neighbour->blockMutex);
                for (int z = z0; z <= z1; z++)
                    for (int y = y0; y <= y1; y++)
                        for (int x = x0; x <= x1; x++) {
                            const int i = Index(x - dx * CHUNK_SIZE, y - dy * CHUNK_SIZE, z - dz * CHUNK_SIZE);
                            grid[paddedIndex(x, y, z)] = neighbour->blocks[i].IsFullBlock();
                            lights[paddedIndex(x, y, z)] = neighbour->light[i];
                        }
            }
        }
    }
    auto isOpaque = [&](const glm::ivec3& p) { return grid[paddedIndex(p.x, p.y, p.z)]; };
    auto blockLightAt = [&](const glm::ivec3& p) { return lights[paddedIndex(p.x, p.y, p.z)] & 0x0F; };
    auto skyLightAt = [&](const glm::ivec3& p) { return lights[paddedIndex(p.x, p.y, p.z)] >> 4; };

    std::vector<uint32_t> vertices;

    for (int x = 0; x < CHUNK_SIZE; ++x) {
        for (int y = 0; y < CHUNK_SIZE; ++y) {
            for (int z = 0; z < CHUNK_SIZE; ++z) {
                const Block& block = local[Index(x, y, z)];
                if (block.IsAir()) continue;

                const bool full = block.IsFullBlock();
                const glm::ivec3 pos(x, y, z);

                for (int face = 0; face < kFaceCount; ++face) {
                    const glm::ivec3 normal(kFaceOffsets[face][0], kFaceOffsets[face][1], kFaceOffsets[face][2]);
                    const glm::ivec3 front = pos + normal;
                    if (full && isOpaque(front)) continue;

                    // Classic voxel AO: each corner is darkened by the two edge
                    // neighbours and the diagonal neighbour in the layer in front of
                    // the face. Two solid edges fully occlude the corner.
                    // Smooth lighting: each corner averages the light of the open
                    // cells among the same four (front, both edges, diagonal).
                    const auto& tangents = kFaceTangents[kFaceAxis[face]];
                    FaceShading shading;
                    for (int a = 0; a < 2; a++) {
                        for (int b = 0; b < 2; b++) {
                            glm::ivec3 ta(0), tb(0);
                            ta[tangents[0]] = a ? 1 : -1;
                            tb[tangents[1]] = b ? 1 : -1;
                            bool side1 = isOpaque(front + ta);
                            bool side2 = isOpaque(front + tb);
                            bool corner = isOpaque(front + ta + tb);
                            const bool cornerHidden = side1 && side2;
                            shading.ao[a][b] = cornerHidden ? 0 : static_cast<uint8_t>(3 - (side1 + side2 + corner));

                            int blockSum = blockLightAt(front), skySum = skyLightAt(front), count = 1;
                            auto addCell = [&](const glm::ivec3& c) { blockSum += blockLightAt(c); skySum += skyLightAt(c); count++; };
                            if (!side1) addCell(front + ta);
                            if (!side2) addCell(front + tb);
                            if (!corner && !cornerHidden) addCell(front + ta + tb);
                            shading.light[a][b] = static_cast<uint8_t>((blockSum + count / 2) / count);
                            shading.sky[a][b] = static_cast<uint8_t>((skySum + count / 2) / count);
                        }
                    }

                    block.AddFaceVertices(vertices, static_cast<Face>(face), x, y, z, shading);
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
        if (hasPendingMesh && pendingMesh.empty()) {
            // Nothing to draw; keep any existing buffers but skip creating new ones.
            vertexCount = 0;
            hasPendingMesh = false;
        }
        else if (hasPendingMesh) {
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
