#pragma once

#include <optional>

#include <glm/glm.hpp>

class BlockAccess;

// Collision queries used by player physics (client) and movement checks (server).
//
// Full blocks collide as unit boxes; quarters (built blocks chiselled) and shapes
// turned sideways or upside down as exactly what they fill. Upright slopes (natural
// ground) are a height field: they never block sideways, but things stand on the
// highest point of their surface under them. Plants don't collide at all. Space in
// chunks that aren't loaded yet counts as solid, so nothing falls out of the world
// while terrain streams in.
namespace Collision {

struct AABB {
    glm::vec3 min, max;
};

// True if the box overlaps any block's solid (not an upright slope's) or unloaded space.
// With `slopesFrom`, upright slopes in cells starting at or above that height block too,
// as what they fill: ground overhead (a slope's underside, met rising into it or
// walking under it), not ground underfoot (which is stood on and walked up).
bool BoxBlocked(const BlockAccess& world, const AABB& box);
bool BoxBlocked(const BlockAccess& world, const AABB& box, float slopesFrom);

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
