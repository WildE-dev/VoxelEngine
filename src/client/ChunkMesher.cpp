#include "client/ChunkMesher.h"
#include "world/Lava.h"
#include "world/Power.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>

void ChunkMesher::Build(const Chunk& chunk, const Chunk::Neighbours& neighbours, std::vector<uint32_t>& vertices, std::vector<uint32_t>& waterVertices) {
    static constexpr int CHUNK_SIZE = Chunk::CHUNK_SIZE;
    auto Index = &Chunk::Index;
    vertices.clear();
    waterVertices.clear();

    // Snapshot our own blocks and light so updates from the main thread can't tear the
    // mesh.
    auto snapshot = std::make_unique<Chunk::Blocks>();
    auto lightSnapshot = std::make_unique<Chunk::Light>();
    {
        std::lock_guard<std::mutex> lock(chunk.blockMutex);
        if (!chunk.data) return; // (nothing in it yet)
        *snapshot = chunk.data->blocks;
        *lightSnapshot = chunk.data->light;
    }
    const auto& local = *snapshot;
    const auto& light = *lightSnapshot;

    // Fast path: an all-air chunk has no faces, whatever its neighbours are.
    // Roughly half of a freshly loaded world is open sky.
    if (std::all_of(local.begin(), local.end(), [](const Block& b) { return b.IsAir(); })) return;

    // Opacity of this chunk plus a one block border taken from the surrounding
    // chunks, addressed with coordinates in [-1, CHUNK_SIZE]. Unloaded neighbours
    // count as transparent; they trigger a rebuild of this chunk once they load.
    static constexpr int P = CHUNK_SIZE + 2;
    auto paddedIndex = [](int x, int y, int z) { return (x + 1) + P * ((y + 1) + P * (z + 1)); };
    auto opaque = std::make_unique<std::array<bool, P * P * P>>();
    auto& grid = *opaque;
    // Packed block + sky light over the same padded region.
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
                grid[paddedIndex(x, y, z)] = b.HidesNeighbours();
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
                const auto& neighbour = neighbours[Chunk::NeighbourIndex(dx, dy, dz)];
                if (!neighbour || !neighbour->IsLoaded() || !neighbour->data) continue;

                int x0, x1, y0, y1, z0, z1;
                span(dx, x0, x1);
                span(dy, y0, y1);
                span(dz, z0, z1);

                std::lock_guard<std::mutex> lock(neighbour->blockMutex);
                for (int z = z0; z <= z1; z++)
                    for (int y = y0; y <= y1; y++)
                        for (int x = x0; x <= x1; x++) {
                            const int i = Index(x - dx * CHUNK_SIZE, y - dy * CHUNK_SIZE, z - dz * CHUNK_SIZE);
                            grid[paddedIndex(x, y, z)] = neighbour->data->blocks[i].HidesNeighbours();
                            lights[paddedIndex(x, y, z)] = neighbour->data->light[i];
                            cells[paddedIndex(x, y, z)] = neighbour->data->blocks[i];
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
    // A partial block's faces lie inside its own cell (a slope's surface, a quarter's
    // top): where the cell in front is solid (a block placed on top), they're lit by
    // the open space in their own cell instead, not by the inside of that block (which
    // is dark, and until the server's new light arrives still lit as the air it was),
    // and shaded as in that block's shadow.
    auto shadeFace = [&](const glm::ivec3& pos, int face, bool withAO, bool partial = false) {
        glm::ivec3 front = pos + glm::ivec3(kFaceOffsets[face][0], kFaceOffsets[face][1], kFaceOffsets[face][2]);
        const bool covered = partial && isOpaque(front); // under (or against) a solid block
        if (covered) front = pos;
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
                // Covered: in the block's shadow, as dark as a corner between blocks.
                if (withAO && covered) shading.ao[a][b] = std::min<uint8_t>(shading.ao[a][b], 1);

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
    // An open water surface is drawn a little below the top of its cell even when the
    // cell is full, so it never lies flush with the tops of blocks beside it (no ties in
    // depth to fight over, and blocks level with the water stand just out of it).
    constexpr int kOpenSurfaceTop = 30; // 32nds
    constexpr int kCornerOfEdge[4][2] = { { 0, 1 }, { 1, 0 }, { 0, 0 }, { 1, 1 } }; // EdgeData order
    const glm::ivec3 up(0, 1, 0);
    // Height of a column's water surface relative to the base of row p.y, in units, or
    // -1000 if the column has no water to connect with near this row.
    constexpr int kNoSurface = -1000;
    // Water in a partial block stands apart from the water beside it (flat at its own
    // level, with a side showing between the two) only when the two levels really
    // differ: a carved block's notch still filling beside a pool. Water at about the
    // same level as its neighbours (a waterlogged slope in a lake, a little off from
    // the water round it) is part of one smooth surface, with no little steps.
    constexpr int kApartUnits = Block::kMaxWaterVolume / 8;
    auto standsApart = [&](const glm::ivec3& q) {
        if (!hasWater(q) || !cellAt(q).IsPartial() || hasWater(q + glm::ivec3(0, 1, 0))) return false;
        const int level = cellAt(q).WaterShownLevel();
        static const glm::ivec3 kSides[4] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        for (const auto& d : kSides) {
            const glm::ivec3 o = q + d;
            if (!hasWater(o)) continue;
            const int theirs = hasWater(o + glm::ivec3(0, 1, 0)) ? 2 * Block::kMaxWaterVolume : cellAt(o).WaterShownLevel();
            if (std::abs(theirs - level) > kApartUnits) return true;
        }
        return false;
    };
    auto columnSurface = [&](const glm::ivec3& q) -> int {
        // Water in a partial block stands flat at its own level, held in by the block:
        // the water around doesn't slope to meet it (a pool beside a carved block that's
        // still filling stays level up to it, its side showing down to the lower water).
        if (standsApart(q)) return kNoSurface;
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
        int sum = 0, n = 0, wetSum = 0, wetN = 0, wetTop = kNoSurface, dry[4], dryN = 0;
        bool dryStepped[4] = {};
        bool meetsLowerWater = false; // a column here is dry, with water just under it
        for (int dx = cx - 1; dx <= cx; dx++) {
            for (int dz = cz - 1; dz <= cz; dz++) {
                const glm::ivec3 q = p + glm::ivec3(dx, 0, dz);
                // Water continues above this row in that column (or sits on a solid block
                // there): the corner must reach the top of the cell. The cell above starts
                // exactly there, and no faces are drawn between water cells, so anything
                // lower leaves a slit between the two rows.
                if ((hasWater(q) || isOpaque(q)) && hasWater(q + up)) return 32;
                if (!hasWater(q) && inGrid(q) && !isOpaque(q) && hasWater(q - up)) meetsLowerWater = true;
                const int surface = columnSurface(q);
                if (surface == kNoSurface) continue;
                // Water's own surface (or the brink of a fall) counts as it is; dry ground
                // only where it lies below that water (the edge tapers down to it).
                const bool wet = hasWater(q) || (isOpaque(q) && hasWater(q + up));
                if (wet) {
                    wetSum += surface;
                    wetN++;
                    wetTop = std::max(wetTop, surface);
                }
                else {
                    dryStepped[dryN] = inGrid(q) && cellAt(q).IsPartial() && cellAt(q).WaterStepped();
                    dry[dryN++] = surface;
                }
            }
        }
        // A dry carved block standing above the water doesn't pull the water up toward
        // its top: the water stays level up to its square sides. (A dry slope does: the
        // water runs up the beach to meet it, so a shore follows the slopes smoothly
        // instead of cutting across their triangles in a sawtooth.)
        for (int i = 0; i < dryN; i++) {
            if (wetN > 0 && dryStepped[i] && dry[i] > wetTop) continue;
            sum += dry[i];
            n++;
        }
        sum += wetSum;
        n += wetN;
        if (n == 0) return 1;
        // Water lying on more water (a lake's top layer) comes right down to the water
        // below where it meets an empty column over it: a thin layer thins out to
        // nothing at its edges, instead of ending in a little wall (which shows as
        // patches while a lake drains).
        const bool onWater = hasWater(p - up);
        if (onWater && meetsLowerWater) return 0;
        // Otherwise the columns' mean, at most the cell's top and at least a 32nd (so a
        // film on dry ground never folds into it).
        const int average = (sum + (sum >= 0 ? n / 2 : -n / 2)) / n;
        return std::clamp((average + kUnitsPer32nd / 2) / kUnitsPer32nd, onWater ? 0 : 1, kOpenSurfaceTop);
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

    auto emitWater = [&](const glm::ivec3& here, const Block& self) {
        uint8_t top[4], fine[4], bottom[4]; // top in eighths, rounded up, less fine (0..3 32nds); bottom in eighths
        int top32[4];
        bool visible = false;
        // With more water directly above, this cell is full and meets it exactly.
        const bool covered = hasWater(here + up);
        const bool selfApart = standsApart(here);
        for (int e = 0; e < 4; e++) {
            // Water in a partial block stands flat at its own level (not sloping up to
            // the water beside it, over the block's quarters).
            top32[e] = covered ? 32
                     : selfApart ? std::clamp((self.WaterShownLevel() + kUnitsPer32nd / 2) / kUnitsPer32nd, 1, kOpenSurfaceTop)
                                        : cornerHeight(here, kCornerOfEdge[e][0], kCornerOfEdge[e][1]);
            top[e] = static_cast<uint8_t>((top32[e] + 3) / 4);
            fine[e] = static_cast<uint8_t>(top[e] * 4 - top32[e]);
            // In a waterlogged block the water sits on the slope.
            bottom[e] = self.IsPartial() ? std::min(self.WaterEdges().GetTopY(e), top[e]) : 0;
            visible |= top32[e] > bottom[e] * 4;
        }
        // Entirely below the slope: nothing shows. But a thin layer on more water whose
        // surface rounds down to the bottom of its cell still draws its top there (the
        // water below doesn't draw one, as it has water above it), or there'd be a hole
        // in the surface down to the floor.
        if (!visible && (self.IsPartial() || !hasWater(here - up))) return;

        for (int face = 0; face < kFaceCount; ++face) {
            const glm::ivec3 n = here + glm::ivec3(kFaceOffsets[face][0], kFaceOffsets[face][1], kFaceOffsets[face][2]);
            // A solid block hides the face it touches. The top only touches the block
            // above when the water reaches the top of the cell; a lower surface shows.
            if (isOpaque(n)) {
                const bool touchesAbove = top32[0] == 32 && top32[1] == 32 && top32[2] == 32 && top32[3] == 32;
                if (face != static_cast<int>(Face::Top) || touchesAbove) continue;
            }
            const Block& neighbour = cellAt(n);
            // Water beside water is one surface, with no face between, except where a
            // partial block's water stands flat at its own level (see below): there the
            // side shows above the lower of the two.
            const bool besideWater = neighbour.HasWater();
            if (besideWater) {
                if (face == static_cast<int>(Face::Top) || face == static_cast<int>(Face::Bottom)) continue;
                if (!standsApart(n) && !selfApart) continue;
            }
            // How high the neighbour's water is drawn at our corner `e` (32nds; -1 if it
            // has none): a partial block's flat level, or other water's surface there,
            // which averages the columns around that corner (not its own level).
            auto theirWater32 = [&](int e) {
                if (!besideWater) return -1;
                if (standsApart(n)) return std::clamp((neighbour.WaterShownLevel() + kUnitsPer32nd / 2) / kUnitsPer32nd, 1, kOpenSurfaceTop);
                return cornerHeight(here, kCornerOfEdge[e][0], kCornerOfEdge[e][1]);
            };
            if (face == static_cast<int>(Face::Bottom) && self.IsPartial()) continue; // the slope is the floor

            Block shape;
            shape.type = BlockType::WATER;
            for (int e = 0; e < 4; e++) {
                shape.edgeData.SetTopY(e, top[e]);
                shape.edgeData.SetBottomY(e, bottom[e]);
            }
            // Adds the face as shaped; with `half` set (0 or 1), only the half of the
            // side by corner edges.ours[half]: the other corner is pulled in to the
            // middle of the side.
            auto addFace = [&](const Block& faceShape, int keepCorner, int dropCorner) {
                const size_t first = waterVertices.size();
                faceShape.AddFaceVertices(waterVertices, static_cast<Face>(face), here.x, here.y, here.z, shadeFace(here, face, false));
                // Fine height: vertices on a corner's top edge get the 32nds that corner
                // sits below its rounded-up top (in the AO bits, 18-19); all others get
                // none. A bottom vertex never matches: a visible corner's top is above
                // its bottom.
                const int tangent = (face == static_cast<int>(Face::Right) || face == static_cast<int>(Face::Left)) ? 2 : 0;
                for (size_t i = first; i < waterVertices.size(); i += 2) {
                    uint32_t position = waterVertices[i];
                    const int cx = static_cast<int>((position >> 24) & 0xFF) - here.x * 8; // 0 or 8
                    const int cz = static_cast<int>((position >> 8) & 0xFF) - here.z * 8;
                    const int y = static_cast<int>((position >> 16) & 0xFF) - 8 - here.y * 8; // eighths within the cell
                    const int e = cx == 0 ? (cz == 0 ? 2 : 0) : (cz == 0 ? 1 : 3);
                    const uint32_t extra = y == top[e] ? fine[e] : 0u;
                    waterVertices[i + 1] = (waterVertices[i + 1] & ~(0x3u << 18)) | (extra << 18);
                    // Half a side: the far corner's vertices move to the middle, and its
                    // top comes down (or up) to the surface's height there, halfway
                    // between the two corners (rounded down, so the side never pokes up
                    // through the surface to be seen from the far side).
                    if (dropCorner >= 0 && e == dropCorner) {
                        const int shift = tangent == 0 ? 24 : 8;
                        const int at = static_cast<int>((position >> shift) & 0xFF) + (((tangent == 0 ? cx : cz) == 0) ? 4 : -4);
                        position = (position & ~(0xFFu << shift)) | (static_cast<uint32_t>(at & 0xFF) << shift);
                        if (y == top[e] && top[e] > faceShape.edgeData.GetBottomY(e)) {
                            const int mid32 = (top32[keepCorner] + top32[dropCorner]) / 2;
                            const int y8 = std::max(static_cast<int>(faceShape.edgeData.GetBottomY(e)), (mid32 + 3) / 4);
                            const int midFine = std::clamp(y8 * 4 - mid32, 0, 3);
                            position = (position & ~(0xFFu << 16)) | (static_cast<uint32_t>((y8 + 8 + here.y * 8) & 0xFF) << 16);
                            waterVertices[i + 1] = (waterVertices[i + 1] & ~(0x3u << 18)) | (static_cast<uint32_t>(midFine) << 18);
                        }
                        waterVertices[i] = position;
                    }
                }
            };
            if (face != static_cast<int>(Face::Top) && face != static_cast<int>(Face::Bottom)) {
                // Against a dry slope, only the part above its surface on the shared
                // edge is exposed (below that is the slope's own solid face), and
                // against water standing lower, only the part above that water.
                const SideEdges edges = sideEdges(face);
                auto exposedFrom = [&](int k) {
                    const int e = edges.ours[k];
                    uint8_t from = bottom[e];
                    if (neighbour.IsPartial()) from = std::max(from, std::min(neighbour.WaterEdges().GetTopY(edges.theirs[k]), top[e]));
                    const int theirs = theirWater32(e);
                    if (theirs >= 0) from = std::max(from, static_cast<uint8_t>(std::min(theirs / 4, static_cast<int>(top[e]))));
                    return from;
                };
                // A stepped block on either side has a step in the middle of the side: two
                // halves, each with its own flat bottom (a straight edge between the two
                // corners would cut across the step and leave a hole).
                const bool steps = (neighbour.IsPartial() && neighbour.WaterStepped()) || (self.IsPartial() && self.WaterStepped());
                if (steps) {
                    for (int k = 0; k < 2; k++) {
                        const uint8_t from = exposedFrom(k);
                        Block halfShape = shape;
                        halfShape.edgeData.SetBottomY(edges.ours[0], from);
                        halfShape.edgeData.SetBottomY(edges.ours[1], from);
                        bool exposed = false;
                        for (int j = 0; j < 2; j++) exposed |= top32[edges.ours[j]] > std::max(from * 4, theirWater32(edges.ours[j]));
                        if (exposed) addFace(halfShape, edges.ours[k], edges.ours[1 - k]);
                    }
                    continue;
                }
                bool exposed = false;
                for (int k = 0; k < 2; k++) {
                    const uint8_t from = exposedFrom(k);
                    shape.edgeData.SetBottomY(edges.ours[k], from);
                    exposed |= top32[edges.ours[k]] > std::max(from * 4, theirWater32(edges.ours[k])); // (none where both stand level)
                }
                if (!exposed) continue;
            }
            addFace(shape, -1, -1);
        }
    };


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

                if (block.IsModel()) continue; // (drawn as a model: Renderer)
                if (block.IsLava()) {
                    // A glowing slab as deep as it is (world/Lava.h): faces against lava or
                    // solid blocks left out. It lights itself.
                    unsigned skip = 0;
                    for (int face = 0; face < kFaceCount; ++face) {
                        const glm::ivec3 n = pos + glm::ivec3(kFaceOffsets[face][0], kFaceOffsets[face][1], kFaceOffsets[face][2]);
                        const Block& other = cellAt(n);
                        const bool up = face == static_cast<int>(Face::Top);
                        if (isOpaque(n) || (other.IsLava() && (up || Lava::HeightEighths(other) >= Lava::HeightEighths(block)))) skip |= 1u << face;
                    }
                    block.AddBoxVertices(vertices, x, y, z, glm::ivec3(0), glm::ivec3(8, Lava::HeightEighths(block), 8), skip, kMaxLight,
                                          static_cast<uint8_t>(skyLightAt(pos)));
                    continue;
                }
                if (block.type == BlockType::PISTON_HEAD) {
                    // A plate across the far end of the cell and a rod back through the
                    // rest to the piston (lit by the light in its own cell).
                    const uint8_t bl = static_cast<uint8_t>(blockLightAt(pos)), sl = static_cast<uint8_t>(skyLightAt(pos));
                    const int face = block.axis % 6, axis = kFaceAxis[face];
                    const bool positive = kFaceOffsets[face][axis] > 0;
                    glm::ivec3 lo(0), hi(8);
                    lo[axis] = positive ? 6 : 0;
                    hi[axis] = positive ? 8 : 2;
                    block.AddBoxVertices(vertices, x, y, z, lo, hi, 0, bl, sl);
                    glm::ivec3 rlo(3), rhi(5);
                    rlo[axis] = positive ? 0 : 2;
                    rhi[axis] = positive ? 6 : 8;
                    block.AddBoxVertices(vertices, x, y, z, rlo, rhi, (1u << face) | (1u << (face ^ 1)), bl, sl);
                    continue;
                }
                if (block.IsPipe() || IsValveType(block.type)) {
                    // (A valve: a length of its pipe, drawn with the pipe's look, and its handle.)
                    const bool valve = IsValveType(block.type);
                    const Block tube = valve ? Block(ValvePipe(block.type)) : block;
                    // A thin tube: a core in the middle of the cell, and an arm out to each
                    // neighbour it joins (another pipe, a pump, a compressor, a machine).
                    // Lit by the light in its own cell (pipes let light through).
                    const uint8_t bl = static_cast<uint8_t>(blockLightAt(pos)), sl = static_cast<uint8_t>(skyLightAt(pos));
                    constexpr int kLo = 2, kHi = 6; // eighths: half a block across
                    unsigned joined = 0, gauges = 0;
                    for (int face = 0; face < kFaceCount; ++face) {
                        const glm::ivec3 n = pos + glm::ivec3(kFaceOffsets[face][0], kFaceOffsets[face][1], kFaceOffsets[face][2]);
                        const Block& other = cellAt(n);
                        // (And an air or fluid pipe out to a pressure gauge fixed to it.)
                        const bool gauge = !valve && (block.type == BlockType::AIR_PIPE || block.type == BlockType::FLUID_PIPE) && other.type == BlockType::PRESSURE_GAUGE &&
                                           other.axis % 6 == face;
                        if (Power::PipeJoins(tube.type, other.type) || gauge) joined |= 1u << face;
                        if (gauge) gauges |= 1u << face;
                    }
                    tube.AddBoxVertices(vertices, x, y, z, glm::ivec3(kLo), glm::ivec3(kHi), joined, bl, sl);
                    for (int face = 0; face < kFaceCount; ++face) {
                        if (!(joined & (1u << face))) continue;
                        const int axis = kFaceAxis[face];
                        const bool positive = kFaceOffsets[face][axis] > 0;
                        glm::ivec3 lo(kLo), hi(kHi);
                        lo[axis] = positive ? kHi : 0;
                        hi[axis] = positive ? 8 : kLo;
                        // (Neither end shows: one meets the core, the other the neighbour. A
                        // gauge's stem is thinner than the pipe, so the end toward it shows.)
                        const unsigned ends = (gauges & (1u << face) ? 0u : (1u << face)) | (1u << (face ^ 1));
                        tube.AddBoxVertices(vertices, x, y, z, lo, hi, ends, bl, sl);
                    }
                    if (valve) {
                        // The handle: a stem up from the tube and a bar across its top, along the
                        // pipe when open, across it when shut.
                        const bool open = IsOpenValve(block.type);
                        if ((joined & 0xCu) != 0 && (joined & 0x33u) == 0) {
                            // A pipe running up and down: the handle on its +x side instead, the
                            // bar up and down when open, level when shut.
                            block.AddBoxVertices(vertices, x, y, z, glm::ivec3(6, 3, 3), glm::ivec3(7, 5, 5), 1u << static_cast<int>(Face::Left), bl, sl);
                            block.AddBoxVertices(vertices, x, y, z, open ? glm::ivec3(7, 1, 3) : glm::ivec3(7, 3, 1), open ? glm::ivec3(8, 7, 5) : glm::ivec3(8, 5, 7), 0, bl, sl);
                        }
                        else {
                            const bool alongX = (joined & 0x3u) != 0 || (joined & 0x30u) == 0; // (the pipe's way, level)
                            const bool barAlongX = open ? alongX : !alongX;
                            block.AddBoxVertices(vertices, x, y, z, glm::ivec3(3, 6, 3), glm::ivec3(5, 7, 5), 1u << static_cast<int>(Face::Bottom), bl, sl);
                            block.AddBoxVertices(vertices, x, y, z, barAlongX ? glm::ivec3(1, 7, 3) : glm::ivec3(3, 7, 1), barAlongX ? glm::ivec3(7, 8, 5) : glm::ivec3(5, 8, 7), 0, bl, sl);
                        }
                    }
                    continue;
                }
                if (block.type == BlockType::LADDER) {
                    // A thin panel of rungs against the wall behind it (away from the way
                    // it faces), seen from both sides.
                    const glm::ivec3 f = block.Facing();
                    glm::ivec3 lo(0), hi(8);
                    if (f.x > 0) hi.x = 1;
                    else if (f.x < 0) lo.x = 7;
                    else if (f.z < 0) lo.z = 7;
                    else hi.z = 1;
                    block.AddBoxVertices(vertices, x, y, z, lo, hi, 0, static_cast<uint8_t>(blockLightAt(pos)), static_cast<uint8_t>(skyLightAt(pos)));
                    continue;
                }
                if (block.IsShell()) {
                    // A see-through cube of rungs: its faces, both ways round (seen from
                    // inside while climbing too), except where a solid block hides them.
                    for (int face = 0; face < kFaceCount; ++face) {
                        const glm::ivec3 front = pos + glm::ivec3(kFaceOffsets[face][0], kFaceOffsets[face][1], kFaceOffsets[face][2]);
                        if (isOpaque(front) || cellAt(front).type == block.type) continue;
                        block.AddFaceVertices(vertices, static_cast<Face>(face), x, y, z, shadeFace(pos, face, true, true));
                    }
                    continue;
                }
                if (block.IsPlant()) {
                    block.AddPlantVertices(vertices, x, y, z, static_cast<uint8_t>(blockLightAt(pos)), static_cast<uint8_t>(skyLightAt(pos)));
                    continue;
                }

                // A shape turned sideways or upside down (Block::ShapeDir): its faces drawn as
                // if upright (a log's grain turned to match), then turned into place about
                // the cell. Each face is lit, corner shadows and all, from the side it ends
                // up facing: its corners take the shading of the corners they end up at.
                if (const int dir = block.ShapeDir(); dir != 0 && !block.IsFullBlock()) {
                    const size_t first = vertices.size();
                    const Block upright = block.Upright();
                    const bool quarters = block.IsStepped();
                    auto offset = [](int f) { return glm::ivec3(kFaceOffsets[f][0], kFaceOffsets[f][1], kFaceOffsets[f][2]); };
                    for (int face = 0; face < kFaceCount; ++face) {
                        const glm::ivec3 n(glm::round(Block::ShapeVectorToCell(dir, glm::vec3(offset(face)))));
                        int worldFace = face;
                        for (int f = 0; f < kFaceCount; f++)
                            if (offset(f) == n) worldFace = f;
                        const auto seen = shadeFace(pos, worldFace, true, true);
                        // Corner [a][b] of the upright face (low or high along its two tangents)
                        // is the world face's corner where that point ends up.
                        const auto& tangents = kFaceTangents[kFaceAxis[face]];
                        const auto& worldTangents = kFaceTangents[kFaceAxis[worldFace]];
                        FaceShading shading;
                        for (int a = 0; a < 2; a++)
                            for (int b = 0; b < 2; b++) {
                                glm::vec3 p(0.5f);
                                p[tangents[0]] = a ? 0.75f : 0.25f;
                                p[tangents[1]] = b ? 0.75f : 0.25f;
                                const glm::vec3 c = Block::ShapeToCell(dir, p);
                                const int wa = c[worldTangents[0]] > 0.5f, wb = c[worldTangents[1]] > 0.5f;
                                shading.ao[a][b] = seen.ao[wa][wb];
                                shading.light[a][b] = seen.light[wa][wb];
                                shading.sky[a][b] = seen.sky[wa][wb];
                            }
                        if (quarters) upright.AddSteppedFaces(vertices, static_cast<Face>(face), x, y, z, shading, true);
                        else upright.AddFaceVertices(vertices, static_cast<Face>(face), x, y, z, shading);
                    }
                    for (size_t i = first; i + 1 < vertices.size(); i += 2) {
                        const uint32_t p = vertices[i], s = vertices[i + 1];
                        // Position: eighths in the chunk (y one block up), see Block's VertexData.
                        const glm::vec3 local(((p >> 24) & 0xFFu) / 8.0f - x, (static_cast<int>((p >> 16) & 0xFFu) - 8) / 8.0f - y, ((p >> 8) & 0xFFu) / 8.0f - z);
                        const glm::vec3 c = Block::ShapeToCell(dir, local);
                        const uint32_t px = static_cast<uint32_t>(std::lround((x + c.x) * 8.0f)), py = static_cast<uint32_t>(std::lround((y + c.y + 1.0f) * 8.0f)),
                                       pz = static_cast<uint32_t>(std::lround((z + c.z) * 8.0f));
                        vertices[i] = ((px & 0xFFu) << 24) | ((py & 0xFFu) << 16) | ((pz & 0xFFu) << 8) | (p & 0xFFu);
                        // Normal: 4 bits an axis.
                        const glm::vec3 normal(((s >> 28) & 0xFu) / 7.5f - 1.0f, ((s >> 24) & 0xFu) / 7.5f - 1.0f, ((s >> 20) & 0xFu) / 7.5f - 1.0f);
                        const glm::vec3 turned = Block::ShapeVectorToCell(dir, normal);
                        auto bits = [](float v) { return static_cast<uint32_t>(std::clamp(std::lround((v + 1.0f) * 7.5f), 0L, 15L)); };
                        vertices[i + 1] = (s & ~(0xFFFu << 20)) | (bits(turned.x) << 28) | (bits(turned.y) << 24) | (bits(turned.z) << 20);
                    }
                    if (block.HasWater()) emitWater(pos, block); // (water standing in it)
                    continue;
                }

                const bool full = block.IsFullBlock();
                const bool stepped = !full && block.IsStepped(); // quarters, not a slope
                const Block& drawn = block; // (every block is drawn inside its own cell)
                for (int face = 0; face < kFaceCount; ++face) {
                    const glm::ivec3 front = pos + glm::ivec3(kFaceOffsets[face][0], kFaceOffsets[face][1], kFaceOffsets[face][2]);
                    if (stepped) {
                        block.AddSteppedFaces(vertices, static_cast<Face>(face), x, y, z, shadeFace(pos, face, true, true), !isOpaque(front));
                        continue;
                    }
                    if (full && isOpaque(front)) continue;
                    // Glass against glass: no face between them (a pane, not a stack of boxes).
                    if (full && block.type == BlockType::GLASS && cellAt(front).type == BlockType::GLASS) continue;
                    drawn.AddFaceVertices(vertices, static_cast<Face>(face), x, y, z, shadeFace(pos, face, true, !full));
                }

                // A waterlogged slope also gets the water above it.
                if (block.HasWater()) emitWater(pos, block);
            }
        }
    }

}
