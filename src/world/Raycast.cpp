#include "world/Raycast.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "world/BlockAccess.h"
#include "world/Coords.h"
#include "world/ModelBoxes.h"
#include "world/SpriteMasks.h"

namespace {

// Amanatides & Woo voxel traversal:
// http://www.cse.chalmers.se/edu/year/2010/course/TDA361/grid.pdf
// Calls visit(block, normal, tEnter, tExit) for each voxel the ray passes through
// (with the distances along the ray where it enters and leaves it) until it returns
// false or maxDistance is reached. `direction` must be normalised.
template <typename Visitor>
void WalkVoxels(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, Visitor visit) {
    glm::ivec3 pos(std::floor(origin.x), std::floor(origin.y), std::floor(origin.z));
    glm::ivec3 step;
    glm::vec3 tDelta, tMax;

    for (int axis = 0; axis < 3; axis++) {
        step[axis] = direction[axis] > 0 ? 1 : -1;
        if (direction[axis] == 0.0f) {
            tDelta[axis] = FLT_MAX;
            tMax[axis] = FLT_MAX;
        }
        else {
            tDelta[axis] = std::abs(1.0f / direction[axis]);
            float dist = step[axis] > 0 ? (pos[axis] + 1 - origin[axis]) : (origin[axis] - pos[axis]);
            tMax[axis] = tDelta[axis] * dist;
        }
    }

    glm::ivec3 normal(0);
    float t = 0.0f;
    while (t <= maxDistance) {
        // Step along whichever axis reaches its next voxel boundary first.
        int axis = tMax.x < tMax.y ? (tMax.x < tMax.z ? 0 : 2) : (tMax.y < tMax.z ? 1 : 2);
        if (!visit(pos, normal, t, std::min(tMax[axis], maxDistance))) return;

        pos[axis] += step[axis];
        t = tMax[axis];
        tMax[axis] += tDelta[axis];
        normal = glm::ivec3(0);
        normal[axis] = -step[axis];
    }
}

// Whether the ray, between tEnter and tExit (inside the block's cell), touches the
// block's actual shape: all of a full block or a plant's cell; a stepped block's four
// quarter-columns; under a slope's surface. `onTop` is set when it meets the shape
// inside the cell (a slope's surface, a quarter's top, a step) rather than right
// where it came into the cell: the face it touched is the block's top, not the side
// of the cell it entered by.
// Where a grass-like plant's picture is on its sprite (two crossed quads along the
// cell's diagonals, see Block::AddPlantVertices): across each quad (u, 0..1) and up from
// its foot (0..1), measured from the textures. False for other plants (aimed at by
// their whole cell).
bool SpriteArea(BlockType type, float& u0, float& u1, float& height) {
    switch (type) {
    case BlockType::TALL_GRASS: u0 = 0.06f, u1 = 0.94f, height = 0.94f; return true;
    case BlockType::FLOWER_RED:
    case BlockType::FLOWER_YELLOW: u0 = 0.3f, u1 = 0.63f, height = 0.88f; return true;
    case BlockType::DEAD_BUSH: u0 = 0.0f, u1 = 1.0f, height = 0.75f; return true;
    case BlockType::SAPLING:
    case BlockType::PINE_SAPLING: u0 = 0.12f, u1 = 0.82f, height = 0.88f; return true;
    case BlockType::CROP: u0 = 0.12f, u1 = 0.88f, height = 0.56f; return true;
    case BlockType::RIPE_WHEAT: u0 = 0.0f, u1 = 1.0f, height = 1.0f; return true;
    default: return false;
    }
}

// Whether the ray, inside the cell, passes through the picture of a grass-like plant
// (rather than the empty space around it): its texture's solid pixels where the
// textures are known, else the rough area SpriteArea gives.
bool HitsSprite(const Block& block, const glm::ivec3& cell, const glm::vec3& origin, const glm::vec3& direction, float tEnter, float tExit, float u0,
                float u1, float height) {
    const glm::vec3 o = origin - glm::vec3(cell);
    const float bottom = -block.PlantDrop();
    // The two diagonals: x = z (u along x) and x + z = 1 (u back along x).
    for (int d = 0; d < 2; d++) {
        const float denom = d == 0 ? direction.x - direction.z : direction.x + direction.z;
        if (std::abs(denom) < 1e-6f) continue;
        const float t = d == 0 ? (o.z - o.x) / denom : (1.0f - o.x - o.z) / denom;
        if (t < tEnter - 1e-4f || t > tExit + 1e-4f) continue;
        const glm::vec3 p = o + direction * t;
        const float u = d == 0 ? p.x : 1.0f - p.x;
        const float up = p.y - bottom;
        if (up < 0.0f || up > 1.0f || u < 0.0f || u > 1.0f) continue;
        if (const auto solid = SpriteMasks::Solid(GetTextureData(block.type).top, u, 1.0f - up)) {
            if (*solid) return true;
            continue;
        }
        if (u >= u0 && u <= u1 && up <= height) return true;
    }
    return false;
}

// `face` (with onTop): which way the face it met inside the cell faces, in the shape's
// own frame (a quarter's top, its side, or the underside of one chiselled from below).
// `tHit`: how far along the ray it met the shape.
bool HitsShape(const Block& block, const glm::ivec3& cell, const glm::vec3& origin, const glm::vec3& direction, float tEnter, float tExit,
               bool& onTop, glm::vec3& face, float& tHit, unsigned joined = 0) {
    onTop = false;
    face = glm::vec3(0.0f, 1.0f, 0.0f);
    tHit = tEnter;
    {
        // Drawn smaller than the cell (a pipe, a shaft, a gauge): its boxes.
        glm::vec3 lo[Block::kMaxModelBoxes], hi[Block::kMaxModelBoxes];
        const int count = block.ModelBoxes(joined, lo, hi);
        for (int i = 0; i < count; i++) {
            float t0 = tEnter, t1 = tExit;
            for (int a = 0; a < 3 && t0 <= t1; a++) {
                const float l = cell[a] + lo[i][a], h = cell[a] + hi[i][a];
                if (std::abs(direction[a]) < 1e-8f) {
                    if (origin[a] < l || origin[a] > h) t0 = t1 + 1.0f;
                    continue;
                }
                float ta = (l - origin[a]) / direction[a], tb = (h - origin[a]) / direction[a];
                if (ta > tb) std::swap(ta, tb);
                t0 = std::max(t0, ta);
                t1 = std::min(t1, tb);
            }
            if (t0 <= t1) {
                tHit = t0;
                return true;
            }
        }
        if (count >= 0) return false;
    }
    if (block.IsFullBlock()) return true;
    if (const int dir = block.ShapeDir(); dir != 0 && block.IsPartial()) {
        // Turned (Block::ShapeDir): the ray into the shape's own upright frame, and tested
        // there as an upright one (the same distances along it).
        const glm::vec3 base(cell);
        Block upright = block;
        upright.axis = 0;
        return HitsShape(upright, cell, Block::CellToShape(dir, origin - base) + base, Block::CellVectorToShape(dir, direction), tEnter, tExit, onTop, face, tHit);
    }
    if (block.type == BlockType::PEBBLES) {
        // Its little stones (where they're drawn: by the cell's place in its chunk).
        const glm::ivec3 local = Coords::LocalOf(cell);
        glm::vec3 lo[5], hi[5];
        const int count = block.PebbleStones(local.x, local.y, local.z, lo, hi);
        for (int i = 0; i < count; i++) {
            float t0 = tEnter, t1 = tExit;
            for (int a = 0; a < 3 && t0 <= t1; a++) {
                const float l = cell[a] + lo[i][a], h = cell[a] + hi[i][a];
                if (std::abs(direction[a]) < 1e-8f) {
                    if (origin[a] < l || origin[a] > h) t0 = t1 + 1.0f;
                    continue;
                }
                float ta = (l - origin[a]) / direction[a], tb = (h - origin[a]) / direction[a];
                if (ta > tb) std::swap(ta, tb);
                t0 = std::max(t0, ta);
                t1 = std::min(t1, tb);
            }
            if (t0 <= t1) {
                tHit = t0;
                return true;
            }
        }
        return false;
    }
    if (block.IsPlant()) {
        float u0, u1, height;
        return SpriteArea(block.type, u0, u1, height) ? HitsSprite(block, cell, origin, direction, tEnter, tExit, u0, u1, height) : true;
    }
    const glm::vec3 base(cell);
    if (block.IsStepped()) {
        // Its quarter-columns, each from its bottom (raised, if chiselled from below) to its
        // top: the nearest one the ray meets, and the face it goes in by.
        constexpr int kEdgeAt[2][2] = { { 2, 0 }, { 1, 3 } };
        float nearest = FLT_MAX;
        for (int qx = 0; qx < 2; qx++)
            for (int qz = 0; qz < 2; qz++) {
                const int e = kEdgeAt[qx][qz];
                const int top = block.edgeData.GetTopY(e), bottom = block.edgeData.GetBottomY(e);
                if (top <= bottom) continue;
                constexpr float kStep = 1.0f / EdgeData::kSteps;
                const glm::vec3 lo = base + glm::vec3(qx * 0.5f, bottom * kStep, qz * 0.5f);
                const glm::vec3 hi = base + glm::vec3(qx * 0.5f + 0.5f, top * kStep, qz * 0.5f + 0.5f);
                float t0 = tEnter, t1 = tExit;
                int enteredBy = -1;
                for (int a = 0; a < 3 && t0 <= t1; a++) {
                    if (std::abs(direction[a]) < 1e-8f) {
                        if (origin[a] < lo[a] || origin[a] > hi[a]) t0 = t1 + 1.0f;
                        continue;
                    }
                    float ta = (lo[a] - origin[a]) / direction[a], tb = (hi[a] - origin[a]) / direction[a];
                    if (ta > tb) std::swap(ta, tb);
                    if (ta > t0) t0 = ta, enteredBy = a;
                    t1 = std::min(t1, tb);
                }
                if (t0 > t1 || t0 >= nearest) continue;
                nearest = t0;
                tHit = t0;
                onTop = t0 > tEnter + 1e-4f;
                if (enteredBy >= 0) {
                    face = glm::vec3(0.0f);
                    face[enteredBy] = direction[enteredBy] > 0.0f ? -1.0f : 1.0f;
                }
            }
        return nearest != FLT_MAX;
    }
    // A slope: sample along the ray through the cell (fine enough for aiming).
    constexpr int kSamples = 24;
    for (int i = 0; i <= kSamples; i++) {
        const float t = tEnter + (tExit - tEnter) * i / kSamples;
        const glm::vec3 p = origin + direction * t - base;
        const float fx = std::clamp(p.x, 0.0f, 1.0f), fz = std::clamp(p.z, 0.0f, 1.0f);
        if (p.y <= block.SurfaceHeight(fx, fz)) {
            onTop = i > 0;
            tHit = t;
            return true;
        }
    }
    return false;
}

} // namespace

std::optional<RaycastHit> Raycast(const BlockAccess& world, const glm::vec3& origin, const glm::vec3& direction, float maxDistance) {
    std::optional<RaycastHit> hit;
    WalkVoxels(origin, direction, maxDistance, [&](const glm::ivec3& pos, const glm::ivec3& normal, float tEnter, float tExit) {
        Block block;
        // Rays pass through water (you click the sea floor, not the surface), and
        // through the open part of a partial block (aiming past a slope or a step).
        bool onTop = false;
        glm::vec3 face(0.0f, 1.0f, 0.0f);
        float tHit = tEnter;
        if (world.GetBlock(pos.x, pos.y, pos.z, block) && !block.IsAir() && !block.IsLiquid() && !block.IsLava() && block.type != BlockType::FIRE &&
            HitsShape(block, pos, origin, direction, tEnter, tExit, onTop, face, tHit, ModelBoxes::Joins(world, pos, block))) {
            // Met the shape inside the cell (over a slope, onto a quarter's top, side or
            // underside): that face, turned the way the shape's turned.
            hit = RaycastHit{ pos, onTop ? glm::ivec3(glm::round(Block::ShapeVectorToCell(block.ShapeDir(), face))) : normal, origin + direction * tHit };
            return false;
        }
        return true;
    });
    return hit;
}

std::vector<glm::ivec3> TraceBlocks(const BlockAccess& world, const glm::vec3& origin, const glm::vec3& direction, float maxDistance) {
    std::vector<glm::ivec3> blocks;
    bool hitSolid = false;
    WalkVoxels(origin, direction, maxDistance, [&](const glm::ivec3& pos, const glm::ivec3&, float, float) {
        Block block;
        if (world.GetBlock(pos.x, pos.y, pos.z, block)) {
            hitSolid |= !block.IsAir() && !block.IsLiquid() && !block.IsLava();
            blocks.push_back(pos);
        }
        return true;
    });
    if (!hitSolid) blocks.clear();
    return blocks;
}
