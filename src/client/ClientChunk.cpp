#include "client/ClientChunk.h"

#include "client/ChunkMesher.h"

void ClientChunk::BuildMesh(const Neighbours& neighbours) {
    std::vector<uint32_t> vertices, waterVertices;
    ChunkMesher::Build(*this, neighbours, vertices, waterVertices);
    ComputeConnectivity();

    std::lock_guard<std::mutex> lock(meshMutex);
    pendingMesh = std::move(vertices);
    pendingWaterMesh = std::move(waterVertices);
    hasPendingMesh = true;
}

void ClientChunk::ComputeConnectivity() {
    // Flood fill each region of open cells, noting which faces of the chunk it
    // touches; every pair of those faces sees each other.
    thread_local Blocks blocks;
    thread_local std::vector<uint8_t> seen;
    thread_local std::vector<int> stack;
    CopyBlocks(blocks);
    seen.assign(VOLUME, 0);
    constexpr int N = CHUNK_SIZE;
    uint64_t result = 0;
    for (int start = 0; start < VOLUME; start++) {
        if (seen[start] || blocks[start].HidesNeighbours()) continue;
        seen[start] = 1;
        stack.clear();
        stack.push_back(start);
        unsigned faces = 0;
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            const int x = i % N, y = (i / N) % N, z = i / (N * N); // Index: x + N * (y + N * z)
            if (x == N - 1) faces |= 1u << 0;
            if (x == 0) faces |= 1u << 1;
            if (y == N - 1) faces |= 1u << 2;
            if (y == 0) faces |= 1u << 3;
            if (z == N - 1) faces |= 1u << 4;
            if (z == 0) faces |= 1u << 5;
            for (const auto& d : kFaceDirections) {
                const int nx = x + d[0], ny = y + d[1], nz = z + d[2];
                if (nx < 0 || ny < 0 || nz < 0 || nx >= N || ny >= N || nz >= N) continue;
                const int j = Index(nx, ny, nz);
                if (seen[j] || blocks[j].HidesNeighbours()) continue;
                seen[j] = 1;
                stack.push_back(j);
            }
        }
        for (int a = 0; a < 6; a++)
            if (faces & (1u << a))
                for (int b = 0; b < 6; b++)
                    if (faces & (1u << b)) result |= 1ull << (a * 6 + b);
        if (result == (1ull << 36) - 1) break; // all connected already
    }
    connectivity.store(result, std::memory_order_release);
}

namespace {

// Uploads vertices into a VAO/VBO pair, creating them on first use. Empty meshes
// don't create GL objects.
void UploadMesh(std::vector<uint32_t>& vertices, GLuint& vao, GLuint& vbo, GLsizei& count) {
    if (vertices.empty()) {
        count = 0;
        return;
    }
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
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(uint32_t), vertices.data(), GL_STATIC_DRAW);
    count = static_cast<GLsizei>(vertices.size() / 2);
    std::vector<uint32_t>().swap(vertices);
}

void DeleteMesh(GLuint& vao, GLuint& vbo, GLsizei& count) {
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    vao = vbo = 0;
    count = 0;
}

} // namespace

bool ClientChunk::UploadPendingMesh() {
    if (IsRemoved()) return false;

    std::lock_guard<std::mutex> lock(meshMutex);
    if (!hasPendingMesh) return false;
    UploadMesh(pendingMesh, vao, vbo, vertexCount);
    UploadMesh(pendingWaterMesh, waterVao, waterVbo, waterVertexCount);
    hasPendingMesh = false;
    return true;
}

void ClientChunk::Draw() const {
    if (vertexCount == 0) return;
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);
}

void ClientChunk::DrawWater() const {
    if (waterVertexCount == 0) return;
    glBindVertexArray(waterVao);
    glDrawArrays(GL_TRIANGLES, 0, waterVertexCount);
}

void ClientChunk::ReleaseGpu() {
    DeleteMesh(vao, vbo, vertexCount);
    DeleteMesh(waterVao, waterVbo, waterVertexCount);
}
