#pragma once

#include <glm/glm.hpp>

#include "world/Collision.h"

class BlockAccess;

struct PlayerInput {
    glm::vec2 move{0.0f}; // x = strafe right, y = forward, each -1..1
    bool jump = false;    // held
    bool sprint = false;
    bool descend = false; // fly mode only
    float yaw = -90.0f;   // degrees, from the camera
    glm::vec3 lookDirection{0.0f, 0.0f, -1.0f};
};

// First person player body: an upright box with gravity, jumping and collision.
// In water it swims: slower, buoyant, jump swims up. In fly mode it moves freely
// through blocks like the old free camera.
class Player
{
public:
    static constexpr float kWidth = 0.6f;
    static constexpr float kHeight = 1.8f;
    static constexpr float kEyeHeight = 1.62f;
    static constexpr float kStepHeight = 0.6f; // climb ledges this high without jumping

    explicit Player(const glm::vec3& feetPosition) : feet(feetPosition) {}

    // Advances physics by deltaTime using fixed substeps.
    void Update(const BlockAccess& world, const PlayerInput& input, float deltaTime);

    // Places the player on the terrain surface of its current column. Returns false
    // if the column isn't loaded yet; call again next frame.
    bool Spawn(const BlockAccess& world);
    bool IsSpawned() const { return spawned; }

    void SetFlying(const BlockAccess& world, bool fly);
    bool IsFlying() const { return flying; }
    bool IsOnGround() const { return onGround; }
    bool IsSwimming() const { return inWater; }
    bool IsHeadUnderwater(const BlockAccess& world) const;

    // Moves the player (a server correction or the spawn point) and stops it.
    void Teleport(const glm::vec3& feetPosition) { feet = feetPosition; velocity = glm::vec3(0.0f); accumulator = 0.0f; }
    // Forget the spawn so Spawn drops the player onto the terrain under it again.
    void ResetSpawn() { spawned = false; resumeHere = false; }
    // Spawn exactly here instead (rejoining a saved world): Spawn waits for the terrain
    // around this point, and only drops onto the surface if it's inside blocks now.
    void ResumeAt(const glm::vec3& feetPosition) { Teleport(feetPosition); spawned = false; resumeHere = true; }

    glm::vec3 GetFeetPosition() const { return feet; }
    glm::vec3 GetEyePosition() const { return feet + glm::vec3(0.0f, kEyeHeight, 0.0f); }
    glm::vec3 GetVelocity() const { return velocity; }
    Collision::AABB GetBounds() const { return BoundsAt(feet); }

    static Collision::AABB BoundsAt(const glm::vec3& feet);

private:
    void Step(const BlockAccess& world, const PlayerInput& input, float dt);
    void StepFlying(const PlayerInput& input, float dt);
    void MoveHorizontal(const BlockAccess& world, int axis, float distance);
    void MoveVertical(const BlockAccess& world, float distance, bool wasOnGround);
    bool InWater(const BlockAccess& world) const;

    glm::vec3 feet;
    glm::vec3 velocity{0.0f};
    bool onGround = false;
    bool flying = false;
    bool spawned = false;
    bool resumeHere = false;
    bool inWater = false;
    bool blockedHorizontally = false; // set by MoveHorizontal during a step
    float accumulator = 0.0f;
};
