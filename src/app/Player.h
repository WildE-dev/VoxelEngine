#pragma once

#include <glm/glm.hpp>

#include "world/Collision.h"

class World;

struct PlayerInput {
    glm::vec2 move{0.0f}; // x = strafe right, y = forward, each -1..1
    bool jump = false;    // held
    bool sprint = false;
    bool descend = false; // fly mode only
    float yaw = -90.0f;   // degrees, from the camera
    glm::vec3 lookDirection{0.0f, 0.0f, -1.0f};
};

// First person player body: an upright box with gravity, jumping and collision.
// In fly mode it moves freely through blocks like the old free camera.
class Player
{
public:
    static constexpr float kWidth = 0.6f;
    static constexpr float kHeight = 1.8f;
    static constexpr float kEyeHeight = 1.62f;
    static constexpr float kStepHeight = 0.6f; // climb ledges this high without jumping

    explicit Player(const glm::vec3& feetPosition) : feet(feetPosition) {}

    // Advances physics by deltaTime using fixed substeps.
    void Update(const World& world, const PlayerInput& input, float deltaTime);

    // Places the player on the terrain surface of its current column. Returns false
    // if the column isn't loaded yet; call again next frame.
    bool Spawn(const World& world);
    bool IsSpawned() const { return spawned; }

    void SetFlying(const World& world, bool fly);
    bool IsFlying() const { return flying; }
    bool IsOnGround() const { return onGround; }

    glm::vec3 GetFeetPosition() const { return feet; }
    glm::vec3 GetEyePosition() const { return feet + glm::vec3(0.0f, kEyeHeight, 0.0f); }
    glm::vec3 GetVelocity() const { return velocity; }
    Collision::AABB GetBounds() const { return BoundsAt(feet); }

    static Collision::AABB BoundsAt(const glm::vec3& feet);

private:
    void Step(const World& world, const PlayerInput& input, float dt);
    void StepFlying(const PlayerInput& input, float dt);
    void MoveHorizontal(const World& world, int axis, float distance);
    void MoveVertical(const World& world, float distance, bool wasOnGround);

    glm::vec3 feet;
    glm::vec3 velocity{0.0f};
    bool onGround = false;
    bool flying = false;
    bool spawned = false;
    float accumulator = 0.0f;
};
