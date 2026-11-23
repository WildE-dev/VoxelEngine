#pragma once

#include <cstdint>
#include <vector>

#include "world/Chunk.h"

// Builds a chunk's vertex data (see Block::AddFaceVertices for the packing): opaque
// geometry, and the translucent water surfaces drawn in their own pass. Reads the
// chunk's blocks and light plus a one block border from its neighbours, each under
// that chunk's lock. Any thread; no GL.
class ChunkMesher
{
public:
    static void Build(const Chunk& chunk, const Chunk::Neighbours& neighbours, std::vector<uint32_t>& vertices,
                      std::vector<uint32_t>& waterVertices);
};
