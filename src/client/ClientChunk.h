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

private:
    // Mesh hand-off from the worker to the GL thread.
    std::mutex meshMutex;
    std::vector<uint32_t> pendingMesh, pendingWaterMesh;
    bool hasPendingMesh = false;

    // GL thread only.
    GLuint vao = 0, vbo = 0, waterVao = 0, waterVbo = 0;
    GLsizei vertexCount = 0, waterVertexCount = 0;

    std::atomic_bool dirty{false};
};
