#include "app/Player.h"

#include <algorithm>
#include <cmath>

#include "world/BlockAccess.h"

namespace {

constexpr float kFixedStep = 1.0f / 120.0f;
constexpr float kMaxFrameTime = 0.1f; // don't try to catch up after long hitches

constexpr float kGravity = 28.0f;
constexpr float kTerminalVelocity = 50.0f;
constexpr float kJumpHeight = 1.25f;
const float kJumpSpeed = std::sqrt(2.0f * kGravity * kJumpHeight);

constexpr float kWalkSpeed = 4.5f;
constexpr float kSprintSpeed = 7.0f;
constexpr float kGroundAcceleration = 60.0f;
constexpr float kAirAcceleration = 12.0f;

constexpr float kFlySpeed = 10.0f;

// Swimming.
constexpr float kSwimSpeedFactor = 0.5f;
constexpr float kSwimAcceleration = 20.0f;
constexpr float kWaterGravityFactor = 0.3f;
constexpr float kMaxSinkSpeed = 3.0f;
constexpr float kSwimUpSpeed = 4.0f;
constexpr float kSwimUpAcceleration = 25.0f;
constexpr float kWaterDrag = 2.0f;      // per second, on vertical speed
constexpr float kClimbOutSpeed = 7.5f;  // swimming into a ledge with jump held

constexpr float kMaxSubstep = 0.2f; // never move further than this per collision test
constexpr float kSkin = 1e-3f;      // gap kept between the box and surfaces
constexpr float kGroundSnap = 0.35f; // stick to the ground walking down slopes and steps

glm::vec2 Approach(glm::vec2 current, glm::vec2 target, float maxDelta) {
    glm::vec2 delta = target - current;
    float length = glm::length(delta);
    if (length <= maxDelta || length < 1e-6f) return target;
    return current + delta / length * maxDelta;
}

} // namespace

bool Player::InWater(const BlockAccess& world) const {
    // Feet or waist below the water surface counts as swimming.
    return world.IsUnderwater(feet + glm::vec3(0.0f, 0.1f, 0.0f)) || world.IsUnderwater(feet + glm::vec3(0.0f, 0.9f, 0.0f));
}

bool Player::IsHeadUnderwater(const BlockAccess& world) const {
    return world.IsUnderwater(GetEyePosition());
}

Collision::AABB Player::BoundsAt(const glm::vec3& feet) {
    const float half = kWidth * 0.5f;
    return { feet - glm::vec3(half, 0.0f, half), feet + glm::vec3(half, kHeight, half) };
}

void Player::Update(const BlockAccess& world, const PlayerInput& input, float deltaTime) {
    accumulator += std::min(deltaTime, kMaxFrameTime);
    while (accumulator >= kFixedStep) {
        Step(world, input, kFixedStep);
        accumulator -= kFixedStep;
    }
}

bool Player::Spawn(const BlockAccess& world) {
    // Terrain heights stay well within this range (see TerrainGenerator).
    constexpr int kTop = 200, kBottom = -64; // mountains reach about 110
    if (resumeHere) {
        // Wait until every cell the body touches has loaded, then stay put if it fits.
        const auto box = BoundsAt(feet);
        for (int x = static_cast<int>(std::floor(box.min.x)); x <= static_cast<int>(std::floor(box.max.x)); x++)
            for (int y = static_cast<int>(std::floor(box.min.y)) - 1; y <= static_cast<int>(std::floor(box.max.y)); y++)
                for (int z = static_cast<int>(std::floor(box.min.z)); z <= static_cast<int>(std::floor(box.max.z)); z++) {
                    Block block;
                    if (!world.GetBlock(x, y, z, block)) return false;
                }
        resumeHere = false;
        if (!Collision::BoxBlocked(world, box)) {
            velocity = glm::vec3(0.0f);
            spawned = true;
            return true;
        }
    }
    const int bx = static_cast<int>(std::floor(feet.x));
    const int bz = static_cast<int>(std::floor(feet.z));
    // Down from the top: past anything not loaded above the loaded range, then through
    // open air to the ground. A gap in the loaded cells on the way down (or no open air
    // seen above the ground) means it isn't all here yet: try again later.
    bool seenLoaded = false;
    for (int y = kTop; y >= kBottom; y--) {
        Block block;
        if (!world.GetBlock(bx, y, bz, block)) {
            if (seenLoaded) return false;
            continue;
        }
        if (!seenLoaded && !block.IsPassable()) return false; // the top of what's loaded is underground
        seenLoaded = true;
        if (block.IsPassable()) continue;

        feet.y = y + block.SurfaceHeight(feet.x - bx, feet.z - bz) + kSkin;
        // Neighbouring full blocks can still stick up into the box; climb out.
        while (Collision::BoxBlocked(world, BoundsAt(feet)) && feet.y < kTop) feet.y = std::floor(feet.y) + 1.0f + kSkin;
        velocity = glm::vec3(0.0f);
        onGround = true;
        spawned = true;
        return true;
    }
    return false;
}

void Player::SetFlying(const BlockAccess& world, bool fly) {
    flying = fly;
    velocity = glm::vec3(0.0f);
    onGround = false;
    if (fly) return;

    // Wherever we stopped flying is a valid place to start falling from, unless
    // we're inside terrain: then move up onto the surface (or wait for it to load).
    spawned = true;
    if (Collision::BoxBlocked(world, GetBounds())) spawned = Spawn(world);
}

void Player::Step(const BlockAccess& world, const PlayerInput& input, float dt) {
    if (flying) {
        StepFlying(input, dt);
        return;
    }
    if (!spawned) return;

    // Horizontal velocity eases toward the wanted direction (relative to where the
    // camera faces, ignoring pitch).
    const float yaw = glm::radians(input.yaw);
    const glm::vec2 forward(std::cos(yaw), std::sin(yaw));
    const glm::vec2 right(-forward.y, forward.x);
    glm::vec2 move = input.move;
    if (glm::length(move) > 1.0f) move = glm::normalize(move);
    const glm::vec2 wish = (forward * move.y + right * move.x) * (input.sprint ? kSprintSpeed : kWalkSpeed);

    inWater = InWater(world);

    const float acceleration = inWater ? kSwimAcceleration : onGround ? kGroundAcceleration : kAirAcceleration;
    glm::vec2 horizontal = Approach({ velocity.x, velocity.z }, inWater ? wish * kSwimSpeedFactor : wish, acceleration * dt);
    velocity.x = horizontal.x;
    velocity.z = horizontal.y;

    if (inWater) {
        // Buoyant and damped; holding jump swims up.
        velocity.y -= kGravity * kWaterGravityFactor * dt;
        velocity.y *= std::exp(-kWaterDrag * dt);
        if (input.jump) velocity.y = std::min(kSwimUpSpeed, velocity.y + kSwimUpAcceleration * dt);
        velocity.y = std::max(velocity.y, -kMaxSinkSpeed);
    }
    else {
        if (input.jump && onGround) {
            velocity.y = kJumpSpeed;
            onGround = false;
        }
        velocity.y = std::max(velocity.y - kGravity * dt, -kTerminalVelocity);
    }

    const bool wasOnGround = onGround;
    blockedHorizontally = false;
    MoveHorizontal(world, 0, velocity.x * dt);
    MoveHorizontal(world, 2, velocity.z * dt);
    // Swimming into a ledge with jump held: push up so we can climb out.
    if (inWater && input.jump && blockedHorizontally) velocity.y = std::max(velocity.y, kClimbOutSpeed);
    MoveVertical(world, velocity.y * dt, wasOnGround);
}

void Player::StepFlying(const PlayerInput& input, float dt) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 forward = glm::normalize(input.lookDirection);
    const glm::vec3 right = glm::normalize(glm::cross(forward, up));
    glm::vec3 direction = forward * input.move.y + right * input.move.x;
    if (input.jump) direction += up;
    if (input.descend) direction -= up;
    const float speed = kFlySpeed * (input.sprint ? 2.0f : 1.0f);
    feet += direction * speed * dt;
    velocity = glm::vec3(0.0f);
}

void Player::MoveHorizontal(const BlockAccess& world, int axis, float distance) {
    const int steps = static_cast<int>(std::ceil(std::abs(distance) / kMaxSubstep));
    const float step = steps ? distance / steps : 0.0f;
    const float half = kWidth * 0.5f;

    for (int i = 0; i < steps; i++) {
        glm::vec3 next = feet;
        next[axis] += step;

        if (Collision::BoxBlocked(world, BoundsAt(next))) {
            // Walk up low ledges (e.g. from a slope onto a full block) without jumping.
            const glm::vec3 lift(0.0f, kStepHeight, 0.0f);
            if (onGround && !Collision::BoxBlocked(world, BoundsAt(feet + lift)) &&
                !Collision::BoxBlocked(world, BoundsAt(next + lift))) {
                bool unknown = false;
                if (auto ground = Collision::GroundHeight(world, BoundsAt(next + lift), 0.0f, kStepHeight, unknown); ground && !unknown) {
                    feet = glm::vec3(next.x, *ground + kSkin, next.z);
                    continue;
                }
            }

            // Stop flush against the block face we ran into.
            if (step > 0) feet[axis] = std::floor(next[axis] + half) - half - kSkin;
            else feet[axis] = std::floor(next[axis] - half) + 1.0f + half + kSkin;
            velocity[axis] = 0.0f;
            blockedHorizontally = true;
            return;
        }

        // Slopes don't block sideways, but one rising more than a step is a wall, and so
        // is one that would lift our head into a ceiling.
        bool unknown = false;
        auto slope = Collision::SlopeHeight(world, next.x, next.z, feet.y, feet.y + kHeight, unknown);
        const bool tooSteep = slope && *slope > feet.y + kStepHeight;
        const bool noHeadroom = slope && *slope > feet.y &&
            Collision::BoxBlocked(world, BoundsAt(glm::vec3(next.x, *slope + kSkin, next.z)));
        if (unknown || tooSteep || noHeadroom) {
            velocity[axis] = 0.0f;
            blockedHorizontally = true;
            return;
        }
        feet = next;
    }
}

void Player::MoveVertical(const BlockAccess& world, float distance, bool wasOnGround) {
    if (distance > 0.0f) {
        // Rising: stop at ceilings.
        const int steps = static_cast<int>(std::ceil(distance / kMaxSubstep));
        for (int i = 0; i < steps; i++) {
            glm::vec3 next = feet + glm::vec3(0.0f, distance / steps, 0.0f);
            if (Collision::BoxBlocked(world, BoundsAt(next))) {
                // Flush under the ceiling, but never lower than we already were.
                feet.y = std::max(feet.y, std::floor(next.y + kHeight) - kHeight - kSkin);
                velocity.y = 0.0f;
                break;
            }
            feet = next;
        }
        // Still allow a slope we're walking up to lift us.
    }

    // Falling, standing, or walking over uneven ground. Look for ground between
    // slightly above the feet (slopes rising under us) and wherever this step ends,
    // plus a little extra when already grounded so we stick to downhill slopes.
    const float fall = std::max(0.0f, -distance);
    // Grounded and not jumping: also accept ground a little below this step's fall,
    // so walking down slopes and off small steps follows the surface instead of
    // briefly falling (which would also make jumping unreliable).
    const bool snapping = wasOnGround && distance <= 0.0f;
    const float below = fall + (snapping ? kGroundSnap : 0.0f);
    bool unknown = false;
    auto ground = Collision::GroundHeight(world, GetBounds(), kStepHeight, below, unknown);

    if (unknown && !ground) {
        // Terrain under us hasn't loaded: hold still rather than fall through it.
        velocity.y = 0.0f;
        onGround = true;
        return;
    }

    const float target = feet.y - fall;
    const float lowest = feet.y - below;
    if (ground && *ground >= lowest - kSkin && velocity.y <= 0.0f) {
        // Never lift into a ceiling, not even by the skin gap (MoveHorizontal keeps us
        // out of spots that are too low to stand in, so this only trims tiny overlaps).
        const float standY = *ground + kSkin;
        if (standY <= feet.y || !Collision::BoxBlocked(world, BoundsAt(glm::vec3(feet.x, standY, feet.z))))
            feet.y = standY;
        velocity.y = 0.0f;
        onGround = true;
    }
    else {
        feet.y = target;
        onGround = false;
    }
}
