#pragma once
// The sizes of things that move and collide (players, creatures, dropped items), from
// Resources/Data/bodies.json:
//   "player": { "width": 0.6, "height": 1.8, "eyeHeight": 1.62,
//               "crouchHeight": 1.45, "crouchEyeHeight": 1.27, "stepHeight": 0.6 }
//       its box (a square footprint `width` across), where its eyes are, crouching
//       (a little under 1.5 high fits a 1.5 gap), and the ledges it walks up
//   "item": { "size": 0.25 }                      a dropped item's box (a cube)
//   "creatures": { "deer": { "width": 0.9, "height": 1.3 }, ... }
//       each creature's box, by its id (see Creatures.h)
// Anything the file doesn't say keeps the game's own size. Loaded at start (the game
// and the server) and again when the file changes.
#include <string>

#include <glm/glm.hpp>

#include "world/Collision.h"

namespace Bodies {

struct Player {
    float width = 0.6f;
    float height = 1.8f;
    float eyeHeight = 1.62f;
    float crouchHeight = 1.45f;
    float crouchEyeHeight = 1.27f;
    float stepHeight = 0.6f;
};

constexpr const char* kPath = "Resources/Data/bodies.json";

// Reads the file; false (with why) if it couldn't. What it sets is used, the rest goes
// back to the game's own.
bool Load(const std::string& path, std::string& error);

const Player& PlayerBody();
// A player's box standing (or crouching) with its feet at `feet`.
Collision::AABB PlayerBox(const glm::vec3& feet, bool crouching = false);
float ItemSize();

} // namespace Bodies
