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
constexpr float kCrouchSpeedFactor = 0.3f; // crouching walks at this share of walking speed
constexpr float kClimbSpeed = 3.0f;        // up a ladder
constexpr float kLadderSlide = 1.5f;       // down one, letting go
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

bool Player::OnLadder(const BlockAccess& world) const {
    const auto box = GetBounds();
    for (int x = static_cast<int>(std::floor(box.min.x)); x <= static_cast<int>(std::floor(box.max.x)); x++)
        for (int y = static_cast<int>(std::floor(box.min.y)); y <= static_cast<int>(std::floor(box.max.y)); y++)
            for (int z = static_cast<int>(std::floor(box.min.z)); z <= static_cast<int>(std::floor(box.max.z)); z++) {
                Block b;
                if (world.GetBlock(x, y, z, b) && b.type == BlockType::LADDER) return true;
            }
    return false;
}

bool Player::IsHeadUnderwater(const BlockAccess& world) const {
    return world.IsUnderwater(GetEyePosition());
}

Collision::AABB Player::BoundsAt(const glm::vec3& feet, float height) {
    const float half = kWidth * 0.5f;
    return { feet - glm::vec3(half, 0.0f, half), feet + glm::vec3(half, height, half) };
}

void Player::Update(const BlockAccess& world, const PlayerInput& input, float deltaTime) {
    accumulator += std::min(deltaTime, kMaxFrameTime);
    while (accumulator >= kFixedStep) {
        Step(world, input, kFixedStep);
        accumulator -= kFixedStep;
    }
}

bool Player::Spawn(const BlockAccess& world) {
    // Terrain heights stay well within 200 to -64 (mountains reach about 110), but the
    // search reaches wherever we are too: a bed far down (the Deep) is below that.
    const int kTop = std::max(200, static_cast<int>(std::floor(feet.y)) + 40);
    const int kBottom = std::min(-64, static_cast<int>(std::floor(feet.y)) - 64);
    if (resumeHere) {
        // Wait until every cell the body touches has loaded, then stay put if it fits.
        const auto box = BoundsAt(feet);
        for (int x = static_cast<int>(std::floor(box.min.x)); x <= static_cast<int>(std::floor(box.max.x)); x++)
            for (int y = static_cast<int>(std::floor(box.min.y)) - 1; y <= static_cast<int>(std::floor(box.max.y)); y++)
                for (int z = static_cast<int>(std::floor(box.min.z)); z <= static_cast<int>(std::floor(box.max.z)); z++) {
                    Block block;
                    if (!world.GetBlock(x, y, z, block)) return false;
                }
        if (!Collision::BoxBlocked(world, box)) {
            resumeHere = false;
            velocity = glm::vec3(0.0f);
            spawned = true;
            return true;
        }
        // In something: climb out, a block at a time, up to 32 (what's up there loaded).
        for (int up = 1; up <= 32; up++) {
            const glm::vec3 at(feet.x, std::floor(feet.y) + up + kSkin, feet.z);
            const auto above = BoundsAt(at);
            bool loaded = true;
            for (int y = static_cast<int>(std::floor(above.min.y)); y <= static_cast<int>(std::floor(above.max.y)) && loaded; y++) {
                Block block;
                loaded = world.GetBlock(static_cast<int>(std::floor(feet.x)), y, static_cast<int>(std::floor(feet.z)), block);
            }
            if (!loaded) return false; // (not here yet: try again later)
            if (!Collision::BoxBlocked(world, above)) {
                resumeHere = false;
                feet = at;
                velocity = glm::vec3(0.0f);
                spawned = true;
                return true;
            }
        }
        resumeHere = false; // (no room near: look down the column instead)
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
    crouching = false;
    height = kHeight;
    eyeHeight = kEyeHeight;
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
    if (boating) {
        StepBoat(world, input, dt);
        return;
    }

    // Horizontal velocity eases toward the wanted direction (relative to where the
    // camera faces, ignoring pitch).
    const float yaw = glm::radians(input.yaw);
    const glm::vec2 forward(std::cos(yaw), std::sin(yaw));
    const glm::vec2 right(-forward.y, forward.x);
    glm::vec2 move = input.move;
    if (glm::length(move) > 1.0f) move = glm::normalize(move);
    inWater = InWater(world);

    // Crouch while it's held (not swimming); stand up again once there's room.
    if (input.crouch && !inWater) {
        crouching = true;
        height = kCrouchHeight;
    }
    else if (crouching && !Collision::BoxBlocked(world, BoundsAt(feet, kHeight))) {
        crouching = false;
        height = kHeight;
    }
    const float targetEye = crouching ? kCrouchEyeHeight : kEyeHeight;
    eyeHeight += (targetEye - eyeHeight) * std::min(1.0f, dt * 18.0f);

    const float speed = crouching ? kWalkSpeed * kCrouchSpeedFactor : input.sprint ? kSprintSpeed : kWalkSpeed;
    const glm::vec2 wish = (forward * move.y + right * move.x) * speed;

    const float acceleration = inWater ? kSwimAcceleration : onGround ? kGroundAcceleration : kAirAcceleration;
    glm::vec2 horizontal = Approach({ velocity.x, velocity.z }, inWater ? wish * kSwimSpeedFactor : wish, acceleration * dt);
    velocity.x = horizontal.x;
    velocity.z = horizontal.y;

    onLadder = !inWater && OnLadder(world);
    if (onLadder) {
        // Climbing: moving or jumping goes up, crouching holds on, otherwise a slow slide
        // down (no falling off a ladder).
        if (input.jump || glm::length(input.move) > 0.1f) velocity.y = kClimbSpeed;
        else if (input.crouch) velocity.y = 0.0f;
        else velocity.y = std::max(velocity.y - kGravity * dt, -kLadderSlide);
        if (velocity.y < -kLadderSlide) velocity.y = -kLadderSlide;
    }
    else if (inWater) {
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

void Player::StepBoat(const BlockAccess& world, const PlayerInput& input, float dt) {
    // The water surface in a column near the feet (-1000: none).
    auto surfaceAt = [&](float x, float z) {
        const int cx = static_cast<int>(std::floor(x)), cz = static_cast<int>(std::floor(z));
        for (int y = static_cast<int>(std::floor(feet.y)) + 1; y >= static_cast<int>(std::floor(feet.y)) - 2; y--) {
            Block b, above;
            if (!world.GetBlock(cx, y, cz, b) || !b.HasWater()) continue;
            if (world.GetBlock(cx, y + 1, cz, above) && above.HasWater()) continue;
            return y + b.WaterShownLevel() / static_cast<float>(Block::kMaxWaterVolume);
        }
        return -1000.0f;
    };
    if (surfaceAt(feet.x, feet.z) < -999.0f) { // (beached or drained: out)
        boating = false;
        return;
    }
    const float yaw = glm::radians(input.yaw);
    const glm::vec2 forward(std::cos(yaw), std::sin(yaw)), right(-forward.y, forward.x);
    glm::vec2 move = input.move;
    if (glm::length(move) > 1.0f) move = glm::normalize(move);
    const glm::vec2 wish = (forward * move.y + right * move.x) * (input.sprint ? 9.0f : 6.5f);
    glm::vec2 h(velocity.x, velocity.z);
    h += (wish - h) * std::min(1.0f, dt * 2.0f); // (a boat glides)
    velocity.x = h.x;
    velocity.z = h.y;
    // Only over water: the bow stops at the shore.
    for (int axis : { 0, 2 }) {
        glm::vec3 next = feet;
        next[axis] += velocity[axis] * dt + (velocity[axis] > 0 ? 0.4f : velocity[axis] < 0 ? -0.4f : 0.0f);
        if (surfaceAt(next.x, next.z) < -999.0f || Collision::BoxBlocked(world, BoundsAt(glm::vec3(next.x, feet.y + 0.1f, next.z), height))) {
            velocity[axis] = 0.0f;
            continue;
        }
        feet[axis] += velocity[axis] * dt;
    }
    // Riding on the surface, bobbing a little.
    const float surface = surfaceAt(feet.x, feet.z);
    if (surface > -999.0f) feet.y += (surface - 0.15f - feet.y) * std::min(1.0f, dt * 6.0f);
    velocity.y = 0.0f;
    onGround = true;
    inWater = false;
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

        if (Collision::BoxBlocked(world, BoundsAt(next, height))) {
            // Walk up low ledges (e.g. from a slope onto a full block) without jumping:
            // find the ground a step up, then check there's room at that height (just
            // there, so a half slab can be stepped onto under a low ceiling).
            const glm::vec3 lift(0.0f, kStepHeight, 0.0f);
            if (onGround) {
                bool unknown = false;
                if (auto ground = Collision::GroundHeight(world, BoundsAt(next + lift, height), 0.0f, kStepHeight, unknown); ground && !unknown) {
                    const float standY = *ground + kSkin;
                    const glm::vec3 up(feet.x, standY, feet.z), there(next.x, standY, next.z);
                    if (standY > feet.y && standY - feet.y <= kStepHeight + kSkin && !Collision::BoxBlocked(world, BoundsAt(up, height)) &&
                        !Collision::BoxBlocked(world, BoundsAt(there, height))) {
                        feet = there;
                        continue;
                    }
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
        auto slope = Collision::SlopeHeight(world, next.x, next.z, feet.y, feet.y + height, unknown);
        const bool tooSteep = slope && *slope > feet.y + kStepHeight;
        const bool noHeadroom = slope && *slope > feet.y &&
            Collision::BoxBlocked(world, BoundsAt(glm::vec3(next.x, *slope + kSkin, next.z), height));
        if (unknown || tooSteep || noHeadroom) {
            velocity[axis] = 0.0f;
            blockedHorizontally = true;
            return;
        }
        // Crouching on the ground: don't step off an edge (there must still be ground
        // under some of the body, within a step's drop).
        if (crouching && onGround) {
            bool unknownGround = false;
            const auto ground = Collision::GroundHeight(world, BoundsAt(next, height), 0.0f, kStepHeight, unknownGround);
            if (!ground || unknownGround) {
                velocity[axis] = 0.0f;
                return;
            }
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
            if (Collision::BoxBlocked(world, BoundsAt(next, height))) {
                // Flush under the ceiling, but never lower than we already were.
                feet.y = std::max(feet.y, std::floor(next.y + height) - height - kSkin);
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
        if (standY <= feet.y || !Collision::BoxBlocked(world, BoundsAt(glm::vec3(feet.x, standY, feet.z), height)))
            feet.y = standY;
        velocity.y = 0.0f;
        onGround = true;
    }
    else {
        feet.y = target;
        onGround = false;
    }
}
