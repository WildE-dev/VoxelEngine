#pragma once

#include <optional>

#include <glm/glm.hpp>

class World;

// Collision queries used by player physics. Main thread only (they use World::GetBlock).
//
// Full blocks collide as unit boxes. Partial (sloped) blocks are treated as a height
// field: they never block sideways, but things stand on their top surface. Space in
// chunks that aren't loaded yet counts as solid, so nothing falls out of the world
// while terrain streams in.
namespace Collision {

struct AABB {
    glm::vec3 min, max;
};

// True if the box overlaps any full block or unloaded space.
bool BoxBlocked(const World& world, const AABB& box);

struct Ground {
    float height = 0.0f;
    bool unknown = false; // touched unloaded space; the result can't be trusted
};

// Highest surface under a box standing at `feet`, searching from feet + above down to
// feet - below. Full block tops count anywhere under the box's footprint; slope
// surfaces are sampled at the footprint's centre. nullopt if there's nothing in range.
std::optional<float> GroundHeight(const World& world, const AABB& box, float above, float below, bool& unknown);

// Height of the slope surface (partial blocks only) under (x, z), searched from yHi
// down to yLo. Used to stop walking into slopes too steep to step up.
std::optional<float> SlopeHeight(const World& world, float x, float z, float yLo, float yHi, bool& unknown);

} // namespace Collision
