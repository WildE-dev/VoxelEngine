#pragma once

#include <optional>
#include <vector>

#include <glm/glm.hpp>

class World;

struct RaycastHit {
    glm::ivec3 block;  // the solid block that was hit
    glm::ivec3 normal; // face normal of the side that was entered (zero if the ray started inside it)
};

// First non-air block along the ray, or nullopt if none within maxDistance.
std::optional<RaycastHit> Raycast(const World& world, const glm::vec3& origin, const glm::vec3& direction, float maxDistance);

// Every block position in a loaded chunk along the ray, up to maxDistance.
// Returns an empty list if the ray never touches a solid block.
std::vector<glm::ivec3> TraceBlocks(const World& world, const glm::vec3& origin, const glm::vec3& direction, float maxDistance);
