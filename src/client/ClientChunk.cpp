#include "client/ClientChunk.h"

#include "client/ChunkMesher.h"

void ClientChunk::BuildMesh(const Neighbours& neighbours) {
    std::vector<uint32_t> vertices, waterVertices;
    ChunkMesher::Build(*this, neighbours, vertices, waterVertices);

    std::lock_guard<std::mutex> lock(meshMutex);
    pendingMesh = std::move(vertices);
    pendingWaterMesh = std::move(waterVertices);
    hasPendingMesh = true;
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

void ClientChunk::UploadPendingMesh() {
    if (IsRemoved()) return;

    std::lock_guard<std::mutex> lock(meshMutex);
    if (!hasPendingMesh) return;
    UploadMesh(pendingMesh, vao, vbo, vertexCount);
    UploadMesh(pendingWaterMesh, waterVao, waterVbo, waterVertexCount);
    hasPendingMesh = false;
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
