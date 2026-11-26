#pragma once

#include <optional>

#include <glm/glm.hpp>

class BlockAccess;
struct Block;

// Collision queries used by player physics (client) and movement checks (server).
//
// Full blocks collide as unit boxes; blocks with collision boxes (pipes, shafts, doors:
// their "collision" in Resources/Data/blocks, see BlockData.h) as those boxes; quarters
// (built blocks chiselled) and shapes turned sideways or upside down as exactly what
// they fill. Upright slopes (natural ground) are a height field: they never block
// sideways, but things stand on the highest point of their surface under them. Blocks
// that aren't solid (plants, open doors, levers, or as the data says) don't collide. Space in
// chunks that aren't loaded yet counts as solid, so nothing falls out of the world
// while terrain streams in.
namespace Collision {

struct AABB {
    glm::vec3 min, max;
};

// True if the box overlaps what one block fills (nothing, if it isn't solid): its
// collision boxes, its quarters or turned shape, or its cell; an upright slope only
// with `slopes`.
bool BlockOverlaps(const BlockAccess& world, const glm::ivec3& cell, const Block& block, const AABB& box, bool slopes);

// True if the box overlaps any block's solid (not an upright slope's) or unloaded space.
// With `slopesFrom`, upright slopes in cells starting at or above that height block too,
// as what they fill: ground overhead (a slope's underside, met rising into it or
// walking under it), not ground underfoot (which is stood on and walked up).
bool BoxBlocked(const BlockAccess& world, const AABB& box);
bool BoxBlocked(const BlockAccess& world, const AABB& box, float slopesFrom);
// Whether a box moved sideways to here is stopped: by whatever BoxBlocked says, or by
// an upright slope under it rising more than `step` above its bottom (one it couldn't
// walk or slide up), or by unloaded space. For creatures and dropped items: the player
// does the same in Player::MoveHorizontal, and more.
bool SideBlocked(const BlockAccess& world, const AABB& box, float step);

struct Ground {
    float height = 0.0f;
    bool unknown = false; // touched unloaded space; the result can't be trusted
};

// Highest surface under a box standing at `feet`, searching from feet + above down to
// feet - below: anything solid under any part of the box's footprint, slopes' surfaces
// included. nullopt if there's nothing in range.
std::optional<float> GroundHeight(const BlockAccess& world, const AABB& box, float above, float below, bool& unknown);

// Height of the upright slopes' surface under (x, z), or its highest point under a
// box's footprint, searched from yHi down to yLo. Used to stop walking into slopes too
// steep to step up.
std::optional<float> SlopeHeight(const BlockAccess& world, float x, float z, float yLo, float yHi, bool& unknown);
std::optional<float> SlopeHeight(const BlockAccess& world, const AABB& box, float yLo, float yHi, bool& unknown);

} // namespace Collision
