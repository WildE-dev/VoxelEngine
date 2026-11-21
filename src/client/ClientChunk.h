#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include <glad/glad.h>

#include "world/Chunk.h"

// A chunk on the client: the blocks and light received from the server, plus its meshes.
//
// Threading contract:
//  - BuildMesh() runs on the client world's mesh worker and hands the result over.
//  - UploadPendingMesh(), Draw(), DrawWater() and ReleaseGpu() must run on the thread
//    that owns the GL context.
//  - The destructor never touches GL, so the last reference may be dropped on any
//    thread; GL objects must be released with ReleaseGpu() first.
class ClientChunk : public Chunk
{
public:
    using Chunk::Chunk;

    // Dirty flag used to de-duplicate mesh rebuild requests.
    // MarkDirty returns true if the chunk was clean (i.e. the caller should queue it).
    bool MarkDirty() { return !dirty.exchange(true); }
    void ClearDirty() { dirty = false; }

    // Mesh worker.
    void BuildMesh(const Neighbours& neighbours);

    // GL thread. Upload the latest meshes from the worker (if any), then draw them.
    // Water is a separate mesh, drawn after all opaque geometry. Returns whether
    // anything was uploaded.
    bool UploadPendingMesh();
    void Draw() const;
    void DrawWater() const;
    void ReleaseGpu();
    bool HasGeometry() const { return vertexCount > 0; }
    bool HasWater() const { return waterVertexCount > 0; }

    // Which of the chunk's six faces can be seen from which through it (through cells
    // that don't hide what's behind them: air, water, plants, partial blocks, glass),
    // for culling chunks hidden behind solid ground (caves under the surface). Bit
    // a * 6 + b is set when face a connects to face b; faces in kFaceDirections order.
    // All set until the first mesh is built (unknown: assume open).
    uint64_t GetConnectivity() const { return connectivity.load(std::memory_order_acquire); }
    static bool Connects(uint64_t connectivity, int a, int b) { return (connectivity >> (a * 6 + b)) & 1u; }
    static constexpr int kFaceDirections[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };

private:
    // Mesh hand-off from the worker to the GL thread.
    std::mutex meshMutex;
    std::vector<uint32_t> pendingMesh, pendingWaterMesh;
    bool hasPendingMesh = false;

    // GL thread only.
    GLuint vao = 0, vbo = 0, waterVao = 0, waterVbo = 0;
    GLsizei vertexCount = 0, waterVertexCount = 0;

    std::atomic_bool dirty{false};
    std::atomic<uint64_t> connectivity{ ~0ull };

public:
    // Works out GetConnectivity from the blocks (BuildMesh does it; tests call it).
    void ComputeConnectivity();
};
