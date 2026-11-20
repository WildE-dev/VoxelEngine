#include "world/Chunk.h"

#include <algorithm>
#include <cmath>


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

void Chunk::SetGenerated(const std::array<Block, VOLUME>& generated) {
    {
        std::lock_guard<std::mutex> lock(blockMutex);
        blocks = generated;
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
        pendingWaterMesh.clear();
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
    // The blocks themselves over the same region: water faces need neighbours' water
    // levels and slope shapes.
    auto blockGrid = std::make_unique<std::array<Block, P * P * P>>();
    auto& cells = *blockGrid;
    grid.fill(false);
    lights.fill(0);

    for (int z = 0; z < CHUNK_SIZE; z++)
        for (int y = 0; y < CHUNK_SIZE; y++)
            for (int x = 0; x < CHUNK_SIZE; x++) {
                const Block& b = local[Index(x, y, z)];
                grid[paddedIndex(x, y, z)] = b.IsFullBlock();
                lights[paddedIndex(x, y, z)] = light[Index(x, y, z)];
                cells[paddedIndex(x, y, z)] = b;
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
                            cells[paddedIndex(x, y, z)] = neighbour->blocks[i];
                        }
            }
        }
    }
    auto isOpaque = [&](const glm::ivec3& p) { return grid[paddedIndex(p.x, p.y, p.z)]; };
    auto blockLightAt = [&](const glm::ivec3& p) { return lights[paddedIndex(p.x, p.y, p.z)] & 0x0F; };
    auto skyLightAt = [&](const glm::ivec3& p) { return lights[paddedIndex(p.x, p.y, p.z)] >> 4; };
    auto inGrid = [](const glm::ivec3& p) { return glm::all(glm::greaterThanEqual(p, glm::ivec3(-1))) && glm::all(glm::lessThanEqual(p, glm::ivec3(CHUNK_SIZE))); };
    auto cellAt = [&](const glm::ivec3& p) -> const Block& { return cells[paddedIndex(p.x, p.y, p.z)]; };
    auto hasWater = [&](const glm::ivec3& p) { return inGrid(p) && cellAt(p).HasWater(); };

    // Per-corner shading for a face. Classic voxel AO: each corner is darkened by the
    // two edge neighbours and the diagonal neighbour in the layer in front of the face;
    // two solid edges fully occlude the corner. Smooth lighting: each corner averages
    // the light of the open cells among the same four (front, both edges, diagonal).
    auto shadeFace = [&](const glm::ivec3& pos, int face, bool withAO) {
        const glm::ivec3 front = pos + glm::ivec3(kFaceOffsets[face][0], kFaceOffsets[face][1], kFaceOffsets[face][2]);
        const auto& tangents = kFaceTangents[kFaceAxis[face]];
        FaceShading shading;
        for (int a = 0; a < 2; a++) {
            for (int b = 0; b < 2; b++) {
                glm::ivec3 ta(0), tb(0);
                ta[tangents[0]] = a ? 1 : -1;
                tb[tangents[1]] = b ? 1 : -1;
                const bool side1 = isOpaque(front + ta);
                const bool side2 = isOpaque(front + tb);
                const bool corner = isOpaque(front + ta + tb);
                const bool cornerHidden = side1 && side2;
                if (withAO) shading.ao[a][b] = cornerHidden ? 0 : static_cast<uint8_t>(3 - (side1 + side2 + corner));

                int blockSum = blockLightAt(front), skySum = skyLightAt(front), count = 1;
                auto addCell = [&](const glm::ivec3& c) { blockSum += blockLightAt(c); skySum += skyLightAt(c); count++; };
                if (!side1) addCell(front + ta);
                if (!side2) addCell(front + tb);
                if (!corner && !cornerHidden) addCell(front + ta + tb);
                shading.light[a][b] = static_cast<uint8_t>((blockSum + count / 2) / count);
                shading.sky[a][b] = static_cast<uint8_t>((skySum + count / 2) / count);
            }
        }
        return shading;
    };

    // --- Water surfaces ---
    // Each surface corner is the average height of the water surface in the columns
    // sharing it, so neighbouring cells meet exactly: the surface is continuous and no
    // faces are drawn between water cells. A column's surface is taken near this row:
    // one row up where that column is taller, one row down where it's lower and open
    // above, so flowing water slopes smoothly between rows instead of breaking into
    // separate boxes. Heights are in 32nds of a block: the mesh geometry has eighths
    // (rounded up), and the 0..3 32nds to take back off go in the vertex bits water
    // doesn't use for AO (water.vert subtracts them). Rounding up keeps a corner's top
    // vertex at least an eighth above the cell's bottom, so the two can be told apart
    // even for a surface only a 32nd high.
    constexpr int kUnitsPer32nd = Block::kMaxWaterVolume / 32;
    constexpr int kFullUnits = Block::kMaxWaterVolume;
    constexpr int kCornerOfEdge[4][2] = { { 0, 1 }, { 1, 0 }, { 0, 0 }, { 1, 1 } }; // EdgeData order
    const glm::ivec3 up(0, 1, 0);
    // Height of a column's water surface relative to the base of row p.y, in units, or
    // -1000 if the column has no water to connect with near this row.
    constexpr int kNoSurface = -1000;
    auto columnSurface = [&](const glm::ivec3& q) -> int {
        if (hasWater(q)) {
            if (!hasWater(q + up)) return cellAt(q).WaterShownLevel();
            if (!hasWater(q + up + up)) return kFullUnits + cellAt(q + up).WaterShownLevel();
            return 2 * kFullUnits; // well above this row
        }
        if (!inGrid(q)) return kNoSurface;
        if (!cellAt(q).CanHoldWater()) {
            // A solid block with water on top (the brink of a waterfall): reach up to it,
            // so the falling curtain meets the water pouring over the edge. Otherwise a
            // solid block is a wall, not a surface.
            if (isOpaque(q) && hasWater(q + up)) return kFullUnits + cellAt(q + up).WaterShownLevel();
            return kNoSurface;
        }
        // Dry ground at this row (a dry slope, or open air on solid ground): the water
        // tapers down to it, so edges of flowing water and puddles slope instead of
        // ending in little walls. (Water at rest is bounded by walls or slopes anyway.)
        if (cellAt(q).IsPartial()) return cellAt(q).WaterFloor();
        const glm::ivec3 below = q - up;
        if (hasWater(below)) return cellAt(below).WaterShownLevel() - kFullUnits; // a lower water surface
        if (inGrid(below) && (isOpaque(below) || cellAt(below).IsPartial())) return 0;
        return kNoSurface; // a drop: don't sag toward it
    };
    auto cornerHeight = [&](const glm::ivec3& p, int cx, int cz) {
        int sum = 0, n = 0;
        for (int dx = cx - 1; dx <= cx; dx++) {
            for (int dz = cz - 1; dz <= cz; dz++) {
                const glm::ivec3 q = p + glm::ivec3(dx, 0, dz);
                // Water continues above this row in that column (or sits on a solid block
                // there): the corner must reach the top of the cell. The cell above starts
                // exactly there, and no faces are drawn between water cells, so anything
                // lower leaves a slit between the two rows.
                if ((hasWater(q) || isOpaque(q)) && hasWater(q + up)) return 32;
                const int surface = columnSurface(q);
                if (surface == kNoSurface) continue;
                sum += surface;
                n++;
            }
        }
        // At least a 32nd so the surface never folds below the cell; at most its top.
        const int average = (sum + (sum >= 0 ? n / 2 : -n / 2)) / n;
        return std::clamp((average + kUnitsPer32nd / 2) / kUnitsPer32nd, 1, 32);
    };
    // Side faces: our two edges on that side, and the neighbour's edges they touch.
    struct SideEdges { int ours[2], theirs[2]; };
    auto sideEdges = [](int face) -> SideEdges {
        switch (static_cast<Face>(face)) {
        case Face::Right: return { { 1, 3 }, { 2, 0 } };
        case Face::Left:  return { { 0, 2 }, { 3, 1 } };
        case Face::Back:  return { { 0, 3 }, { 2, 1 } };
        default:          return { { 1, 2 }, { 3, 0 } }; // Front
        }
    };

    std::vector<uint32_t> waterVertices;
    auto emitWater = [&](const glm::ivec3& here, const Block& self) {
        uint8_t top[4], fine[4], bottom[4]; // top in eighths, rounded up, less fine (0..3 32nds); bottom in eighths
        int top32[4];
        bool visible = false;
        // With more water directly above, this cell is full and meets it exactly.
        const bool covered = hasWater(here + up);
        for (int e = 0; e < 4; e++) {
            top32[e] = covered ? 32 : cornerHeight(here, kCornerOfEdge[e][0], kCornerOfEdge[e][1]);
            top[e] = static_cast<uint8_t>((top32[e] + 3) / 4);
            fine[e] = static_cast<uint8_t>(top[e] * 4 - top32[e]);
            // In a waterlogged block the water sits on the slope.
            bottom[e] = self.IsPartial() ? std::min(self.edgeData.GetTopY(e), top[e]) : 0;
            visible |= top32[e] > bottom[e] * 4;
        }
        if (!visible) return; // entirely below the slope

        for (int face = 0; face < kFaceCount; ++face) {
            const glm::ivec3 n = here + glm::ivec3(kFaceOffsets[face][0], kFaceOffsets[face][1], kFaceOffsets[face][2]);
            // A solid block hides the face it touches. The top only touches the block
            // above when the water reaches the top of the cell; a lower surface shows.
            if (isOpaque(n)) {
                const bool touchesAbove = top32[0] == 32 && top32[1] == 32 && top32[2] == 32 && top32[3] == 32;
                if (face != static_cast<int>(Face::Top) || touchesAbove) continue;
            }
            const Block& neighbour = cellAt(n);
            if (neighbour.HasWater()) continue;                                      // continuous water: no face
            if (face == static_cast<int>(Face::Bottom) && self.IsPartial()) continue; // the slope is the floor

            Block shape;
            shape.type = BlockType::WATER;
            for (int e = 0; e < 4; e++) {
                shape.edgeData.SetTopY(e, top[e]);
                shape.edgeData.SetBottomY(e, bottom[e]);
            }
            if (face != static_cast<int>(Face::Top) && face != static_cast<int>(Face::Bottom)) {
                // Against a dry slope, only the part above its surface on the shared
                // edge is exposed (below that is the slope's own solid face).
                const SideEdges edges = sideEdges(face);
                bool exposed = false;
                for (int k = 0; k < 2; k++) {
                    const int e = edges.ours[k];
                    uint8_t from = bottom[e];
                    if (neighbour.IsPartial()) from = std::max(from, std::min(neighbour.edgeData.GetTopY(edges.theirs[k]), top[e]));
                    shape.edgeData.SetBottomY(e, from);
                    exposed |= top32[e] > from * 4;
                }
                if (!exposed) continue;
            }
            const size_t first = waterVertices.size();
            shape.AddFaceVertices(waterVertices, static_cast<Face>(face), here.x, here.y, here.z, shadeFace(here, face, false));

            // Fine height: vertices on a corner's top edge get the 32nds that corner sits
            // below its rounded-up top (in the AO bits, 18-19); all others get none. A
            // bottom vertex never matches: a visible corner's top is above its bottom.
            for (size_t i = first; i < waterVertices.size(); i += 2) {
                const uint32_t position = waterVertices[i];
                const int cx = static_cast<int>((position >> 24) & 0xFF) - here.x * 8; // 0 or 8
                const int cz = static_cast<int>((position >> 8) & 0xFF) - here.z * 8;
                const int y = static_cast<int>((position >> 16) & 0xFF) - 8 - here.y * 8; // eighths within the cell
                const int e = cx == 0 ? (cz == 0 ? 2 : 0) : (cz == 0 ? 1 : 3);
                const uint32_t extra = y == top[e] ? fine[e] : 0u;
                waterVertices[i + 1] = (waterVertices[i + 1] & ~(0x3u << 18)) | (extra << 18);
            }
        }
    };

    std::vector<uint32_t> vertices;

    for (int x = 0; x < CHUNK_SIZE; ++x) {
        for (int y = 0; y < CHUNK_SIZE; ++y) {
            for (int z = 0; z < CHUNK_SIZE; ++z) {
                const Block& block = local[Index(x, y, z)];
                if (block.IsAir()) continue;
                const glm::ivec3 pos(x, y, z);

                if (block.IsLiquid()) {
                    emitWater(pos, block);
                    continue;
                }

                if (block.IsPlant()) {
                    block.AddPlantVertices(vertices, x, y, z, static_cast<uint8_t>(blockLightAt(pos)), static_cast<uint8_t>(skyLightAt(pos)));
                    continue;
                }

                const bool full = block.IsFullBlock();
                for (int face = 0; face < kFaceCount; ++face) {
                    const glm::ivec3 front = pos + glm::ivec3(kFaceOffsets[face][0], kFaceOffsets[face][1], kFaceOffsets[face][2]);
                    if (full && isOpaque(front)) continue;
                    block.AddFaceVertices(vertices, static_cast<Face>(face), x, y, z, shadeFace(pos, face, true));
                }

                // A waterlogged slope also gets the water above it.
                if (block.HasWater()) emitWater(pos, block);
            }
        }
    }

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

void Chunk::UploadPendingMesh() {
    if (removed) return;

    std::lock_guard<std::mutex> lock(meshMutex);
    if (!hasPendingMesh) return;
    UploadMesh(pendingMesh, vao, vbo, vertexCount);
    UploadMesh(pendingWaterMesh, waterVao, waterVbo, waterVertexCount);
    hasPendingMesh = false;
}

void Chunk::Draw() const {
    if (vertexCount == 0) return;
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);
}

void Chunk::DrawWater() const {
    if (waterVertexCount == 0) return;
    glBindVertexArray(waterVao);
    glDrawArrays(GL_TRIANGLES, 0, waterVertexCount);
}

void Chunk::ReleaseGpu() {
    DeleteMesh(vao, vbo, vertexCount);
    DeleteMesh(waterVao, waterVbo, waterVertexCount);
}
