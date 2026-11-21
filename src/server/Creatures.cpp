#include "server/Creatures.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "server/ServerWorld.h"
#include "world/Collision.h"
#include "world/Coords.h"

namespace {

constexpr float kGravity = 25.0f;
constexpr float kJumpSpeed = 7.8f; // clears one block
constexpr float kSightRange = 16.0f;
constexpr int kMaxAnimalsNear = 6, kMaxHostilesNear = 5;
constexpr float kNearRange = 64.0f;
constexpr float kMinSpawnDistance = 12.0f;

Collision::AABB BoxAt(const glm::vec3& feet, const CreatureSpec& spec) {
    return { feet - glm::vec3(spec.halfWidth, 0.0f, spec.halfWidth), feet + glm::vec3(spec.halfWidth, spec.height, spec.halfWidth) };
}

float HorizontalDistance(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(glm::vec2(a.x - b.x, a.z - b.z));
}

// Light at a cell (block, sky), from its chunk's published light; nullopt if unloaded.
bool LightAt(ServerWorld& world, const glm::ivec3& p, uint8_t& block, uint8_t& sky) {
    auto chunk = world.FindChunk(Coords::ChunkOf(p));
    if (!chunk || !chunk->IsLoaded()) return false;
    static thread_local std::unique_ptr<Chunk::Light> light = std::make_unique<Chunk::Light>();
    chunk->CopyPublishedLight(*light);
    const glm::ivec3 l = Coords::LocalOf(p);
    const uint8_t v = (*light)[Chunk::Index(l.x, l.y, l.z)];
    block = v & 0x0F;
    sky = v >> 4;
    return true;
}

// A cell a creature could stand in: room for its height above a floor.
bool CanStand(const BlockAccess& world, const glm::ivec3& feet, int height, bool& floorIsGrass, bool* floorIsLeaves = nullptr) {
    Block floor, b;
    if (!world.GetBlock(feet.x, feet.y - 1, feet.z, floor) || !floor.IsFullBlock()) return false;
    for (int y = 0; y < height; y++)
        if (!world.GetBlock(feet.x, feet.y + y, feet.z, b) || !b.IsPassable() || b.HasWater()) return false;
    floorIsGrass = IsGrassType(floor.type);
    if (floorIsLeaves) *floorIsLeaves = IsLeavesType(floor.type);
    return true;
}

} // namespace

uint16_t CreatureSystem::Spawn(CreatureType type, const glm::vec3& feet) {
    if (creatures.size() >= kMaxCreatures || type == CreatureType::Count) return 0;
    Creature c;
    c.id = nextId++;
    if (nextId == 0) nextId = 1;
    c.type = type;
    c.feet = feet;
    c.health = SpecOf(type).maxHealth;
    c.yaw = std::uniform_real_distribution<float>(0.0f, 360.0f)(rng);
    creatures.push_back(c);
    return c.id;
}

const CreatureSystem::Creature* CreatureSystem::Find(uint16_t id) const {
    for (const auto& c : creatures)
        if (c.id == id) return &c;
    return nullptr;
}

bool CreatureSystem::Attack(uint16_t id, const glm::vec3& eye, Tool tool, int tier, double now, bool& killed) {
    killed = false;
    auto it = std::find_if(creatures.begin(), creatures.end(), [&](const Creature& c) { return c.id == id; });
    if (it == creatures.end()) return false;
    Creature& c = *it;
    const CreatureSpec& spec = SpecOf(c.type);
    // Distance from the eye to the creature's box.
    const Collision::AABB box = BoxAt(c.feet, spec);
    const glm::vec3 nearest = glm::clamp(eye, box.min, box.max);
    if (glm::distance(eye, nearest) > Combat::kReach + 0.5f) return false; // (slack for movement in flight)
    return Damage(id, Combat::Damage(tool, tier), eye, now, killed);
}

bool CreatureSystem::Damage(uint16_t id, int damage, const glm::vec3& from, double now, bool& killed) {
    killed = false;
    auto it = std::find_if(creatures.begin(), creatures.end(), [&](const Creature& c) { return c.id == id; });
    if (it == creatures.end()) return false;
    Creature& c = *it;
    const glm::vec3 eye = from;
    c.health -= damage;
    c.hurtUntil = now + 0.3;
    // Knocked back, away from the hitter.
    glm::vec2 away(c.feet.x - eye.x, c.feet.z - eye.z);
    away = glm::length(away) > 1e-3f ? glm::normalize(away) : glm::vec2(1.0f, 0.0f);
    c.velocity.x = away.x * 6.0f;
    c.velocity.z = away.y * 6.0f;
    c.velocity.y = 5.0f;
    c.onGround = false;
    if (c.type == CreatureType::Deer || c.type == CreatureType::Bat) {
        c.fleeUntil = now + 5.0;
        c.fleeFrom = eye;
    }
    if (c.health <= 0) {
        killed = true;
        creatures.erase(it);
    }
    return true;
}

Collision::AABB CreatureSystem::Box(const Creature& c) { return BoxAt(c.feet, SpecOf(c.type)); }

void CreatureSystem::Provoke(uint16_t id, uint16_t player, double now) {
    for (auto& c : creatures)
        if (c.id == id && c.type == CreatureType::Wolf && c.owner == 0) {
            c.angryAt = player;
            c.angryUntil = now + 20.0;
        }
}

bool CreatureSystem::Tame(uint16_t id, uint16_t player) {
    for (auto& c : creatures)
        if (c.id == id && c.type == CreatureType::Wolf && c.owner == 0) {
            c.owner = player;
            c.angryAt = 0;
            c.health = SpecOf(c.type).maxHealth;
            return true;
        }
    return false;
}

bool CreatureSystem::Burning(ServerWorld& world, const Creature& c, float daylight) const {
    if (!SpecOf(c.type).hostile || daylight < 0.5f) return false;
    uint8_t block = 0, sky = 0;
    const glm::ivec3 head(glm::floor(c.feet + glm::vec3(0.0f, SpecOf(c.type).height - 0.2f, 0.0f)));
    return LightAt(world, head, block, sky) && sky >= 14; // under the open sky
}

void CreatureSystem::Think(Creature& c, const std::vector<Player>& players, double now, const HurtPlayer& hurt, glm::vec2& move, float& speed) {
    const CreatureSpec& spec = SpecOf(c.type);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    // Wandering: every few seconds, a new direction or a rest.
    auto wander = [&] {
        if (now >= c.nextDecision) {
            c.nextDecision = now + 2.0 + 4.0 * unit(rng);
            if (unit(rng) < 0.45f) c.wander = glm::vec2(0.0f);
            else {
                const float a = unit(rng) * 6.2831853f;
                c.wander = glm::vec2(std::cos(a), std::sin(a));
            }
        }
        move = c.wander;
        speed = spec.walkSpeed;
    };

    if (c.type == CreatureType::Wolf) {
        auto chaseAndBite = [&](const glm::vec3& at, auto bite) {
            glm::vec2 to(at.x - c.feet.x, at.z - c.feet.z);
            const float reach = spec.halfWidth + 0.3f + spec.attackReach;
            if (glm::length(to) > 0.4f) move = glm::normalize(to);
            speed = glm::length(to) > reach * 0.7f ? spec.runSpeed : 0.0f;
            if (glm::length(to) <= reach && std::abs(at.y - c.feet.y) < 1.8f && now >= c.nextAttack) {
                c.nextAttack = now + 0.9;
                bite();
            }
        };
        const Player* owner = nullptr;
        for (const auto& p : players)
            if (p.id == c.owner) owner = &p;
        if (c.owner != 0 && owner) {
            // Tame: fights the hostile creature nearest its owner (within 10 blocks),
            // else keeps close, and catches up if left far behind.
            Creature* foe = nullptr;
            float best = 10.0f;
            for (auto& other : creatures)
                if (SpecOf(other.type).hostile && other.health > 0) {
                    const float d = glm::distance(other.feet, owner->feet);
                    if (d < best) best = d, foe = &other;
                }
            if (foe) {
                chaseAndBite(foe->feet, [&] {
                    foe->health -= 4;
                    foe->hurtUntil = now + 0.3;
                });
                return;
            }
            const float d = HorizontalDistance(owner->feet, c.feet);
            if (d > 24.0f) {
                c.feet = owner->feet + glm::vec3(1.0f, 0.5f, 1.0f); // (caught up)
                c.velocity = glm::vec3(0.0f);
            }
            if (d > 3.0f) {
                move = glm::normalize(glm::vec2(owner->feet.x - c.feet.x, owner->feet.z - c.feet.z));
                speed = d > 8.0f ? spec.runSpeed : spec.walkSpeed * 1.5f;
            }
            return;
        }
        // Wild and provoked: after whoever hit it, for a while.
        if (c.angryAt != 0 && now < c.angryUntil)
            for (const auto& p : players)
                if (p.id == c.angryAt && p.vulnerable) {
                    chaseAndBite(p.feet, [&] { hurt(p.id, spec.attackDamage, "was mauled by a wolf"); });
                    return;
                }
        wander();
        return;
    }

    if (c.type == CreatureType::Bat) {
        // Flits: a new direction every second or so (never resting).
        if (now >= c.nextDecision) {
            c.nextDecision = now + 0.6 + unit(rng);
            const float a = unit(rng) * 6.2831853f;
            c.wander = glm::vec2(std::cos(a), std::sin(a));
        }
        move = c.wander;
        speed = now < c.fleeUntil ? spec.runSpeed : spec.walkSpeed;
        return;
    }

    if (c.type == CreatureType::Deer) {
        if (now < c.fleeUntil) {
            glm::vec2 away(c.feet.x - c.fleeFrom.x, c.feet.z - c.fleeFrom.z);
            move = glm::length(away) > 1e-3f ? glm::normalize(away) : glm::vec2(1.0f, 0.0f);
            speed = spec.runSpeed;
        }
        else wander();
        return;
    }

    // Ghoul: after the nearest survival player in sight range.
    const Player* target = nullptr;
    float best = horde ? 2.0f * kSightRange : kSightRange;
    for (const auto& p : players) {
        if (!p.vulnerable || std::abs(p.feet.y - c.feet.y) > 8.0f) continue;
        const float d = HorizontalDistance(p.feet, c.feet);
        if (d < best) best = d, target = &p;
    }
    if (!target) {
        wander();
        return;
    }
    glm::vec2 to(target->feet.x - c.feet.x, target->feet.z - c.feet.z);
    if (c.type == CreatureType::Skeleton) {
        // Keeps 7 to 14 blocks off, edging sideways, and shoots every couple of seconds.
        const float d = glm::length(to);
        const glm::vec2 toward = d > 1e-3f ? to / d : glm::vec2(1.0f, 0.0f);
        if (d < 7.0f) move = -toward, speed = spec.runSpeed;
        else if (d > 14.0f) move = toward, speed = spec.runSpeed;
        else move = glm::vec2(-toward.y, toward.x) * (std::sin(static_cast<float>(now) * 0.7f + c.id) > 0.0f ? 1.0f : -1.0f), speed = spec.walkSpeed * 0.5f;
        c.yaw = glm::degrees(std::atan2(toward.y, toward.x)); // (faces its target even backing off)
        if (d > 3.0f && d < 20.0f && now >= c.nextAttack && shooter) {
            c.nextAttack = now + 2.2 + unit(rng);
            const glm::vec3 eye = c.feet + glm::vec3(0.0f, 1.55f, 0.0f);
            // Aimed high by as much as the arrow will drop on the way (its speed and
            // gravity are the server's: 25.6 and 18 blocks a second).
            const float flight = d / 25.6f;
            const glm::vec3 aim = target->feet + glm::vec3(0.0f, 1.0f + 0.5f * 18.0f * flight * flight, 0.0f);
            shooter(eye + glm::normalize(aim - eye) * 0.6f, glm::normalize(aim - eye));
        }
        return;
    }
    const float reach = spec.halfWidth + 0.3f + spec.attackReach; // to the player's box edge, plus its reach
    if (glm::length(to) > 0.6f) move = glm::normalize(to);
    speed = glm::length(to) > reach * 0.7f ? spec.runSpeed : 0.0f;
    if (glm::length(to) <= reach && std::abs(target->feet.y - c.feet.y) < 1.8f && now >= c.nextAttack) {
        c.nextAttack = now + 1.0;
        hurt(target->id, spec.attackDamage, "was killed by a ghoul");
    }
}

void CreatureSystem::Move(ServerWorld& world, Creature& c, const glm::vec2& move, float speed, float dt) {
    const CreatureSpec& spec = SpecOf(c.type);
    if (c.type == CreatureType::Bat) {
        // Flies: no gravity, a fluttering wander in three dimensions, bouncing off what
        // it meets.
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        const glm::vec3 want(move.x * speed, std::sin(static_cast<float>(clock) * 2.3f + c.id) * 1.2f + unit(rng) * 0.6f, move.y * speed);
        c.velocity += (want - c.velocity) * std::min(1.0f, dt * 3.0f);
        if (glm::length(move) > 0.1f) c.yaw = glm::degrees(std::atan2(move.y, move.x));
        for (int axis = 0; axis < 3; axis++) {
            glm::vec3 next = c.feet;
            next[axis] += c.velocity[axis] * dt;
            if (Collision::BoxBlocked(world, BoxAt(next, spec))) {
                c.velocity[axis] = -c.velocity[axis] * 0.5f;
                if (axis != 1) c.wander = -c.wander;
            }
            else c.feet = next;
        }
        c.onGround = false;
        return;
    }
    // Steer toward the wanted velocity: firmly on the ground, barely in the air (so a
    // knockback carries).
    const glm::vec2 want = move * speed;
    const float grip = std::min(1.0f, dt * (c.onGround ? 10.0f : 1.0f));
    c.velocity.x += (want.x - c.velocity.x) * grip;
    c.velocity.z += (want.y - c.velocity.z) * grip;
    if (glm::length(want) > 0.1f) {
        const float target = glm::degrees(std::atan2(move.y, move.x));
        float diff = std::fmod(target - c.yaw + 540.0f, 360.0f) - 180.0f;
        c.yaw += diff * std::min(1.0f, dt * 8.0f);
    }

    // In water: float up.
    Block here;
    const glm::ivec3 body(glm::floor(c.feet + glm::vec3(0.0f, 0.4f, 0.0f)));
    const bool swimming = world.GetBlock(body.x, body.y, body.z, here) && here.HasWater();
    c.velocity.y = swimming ? std::min(c.velocity.y + 20.0f * dt, 2.0f) : std::max(c.velocity.y - kGravity * dt, -30.0f);

    // Sideways, one axis at a time; a full block in the way: hop if there's room.
    for (int axis : { 0, 2 }) {
        const float step = c.velocity[axis] * dt;
        if (step == 0.0f) continue;
        glm::vec3 next = c.feet;
        next[axis] += step;
        if (!Collision::BoxBlocked(world, BoxAt(next, spec))) {
            c.feet = next;
            continue;
        }
        const glm::vec3 up(0.0f, 1.05f, 0.0f);
        if ((c.onGround || swimming) && !Collision::BoxBlocked(world, BoxAt(c.feet + up, spec)) && !Collision::BoxBlocked(world, BoxAt(next + up, spec)))
            c.velocity.y = kJumpSpeed;
        c.velocity[axis] = 0.0f;
    }

    // Up and down: land on whatever's below (full blocks or slopes), bump heads.
    const float dy = c.velocity.y * dt;
    if (dy > 0.0f) {
        glm::vec3 next = c.feet + glm::vec3(0.0f, dy, 0.0f);
        if (Collision::BoxBlocked(world, BoxAt(next, spec))) c.velocity.y = 0.0f;
        else c.feet = next;
        c.onGround = false;
        return;
    }
    bool unknown = false;
    const auto ground = Collision::GroundHeight(world, BoxAt(c.feet, spec), 0.55f, -dy + 0.05f, unknown);
    if (unknown) {
        c.velocity.y = 0.0f; // terrain not loaded: hold still
        return;
    }
    if (ground && c.feet.y + dy <= *ground) {
        c.feet.y = *ground;
        c.velocity.y = 0.0f;
        c.onGround = true;
    }
    else {
        c.feet.y += dy;
        c.onGround = false;
    }
}

void CreatureSystem::SpawnNaturally(ServerWorld& world, const std::vector<Player>& players, float daylight) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    auto farFromPlayers = [&](const glm::vec3& p) {
        for (const auto& pl : players)
            if (glm::distance(pl.feet, p) < kMinSpawnDistance) return false;
        return true;
    };
    for (const auto& player : players) {
        int animals = 0, hostiles = 0;
        for (const auto& c : creatures)
            if (glm::distance(c.feet, player.feet) < kNearRange) (SpecOf(c.type).hostile ? hostiles : animals)++;

        // A herd of deer on grass somewhere around, with room to stand: a few tries.
        for (int attempt = 0; attempt < 4 && animals < kMaxAnimalsNear; attempt++) {
            const float a = unit(rng) * 6.2831853f, d = 20.0f + 28.0f * unit(rng);
            const int x = static_cast<int>(std::floor(player.feet.x + std::cos(a) * d));
            const int z = static_cast<int>(std::floor(player.feet.z + std::sin(a) * d));
            const int top = static_cast<int>(player.feet.y) + 24;
            for (int y = top; y > top - 48; y--) {
                bool grass = false, leaves = false;
                if (!CanStand(world, { x, y, z }, 2, grass, &leaves)) continue;
                if (leaves) continue; // a treetop: look under it
                if (!grass) break;
                uint8_t block = 0, sky = 0;
                if (!LightAt(world, { x, y, z }, block, sky) || sky < 12) break; // out in the open
                // Now and then a pair of wolves instead of a herd of deer.
                const bool wolves = unit(rng) < 0.15f;
                const int herd = wolves ? 2 : 1 + static_cast<int>(unit(rng) * 3.0f);
                for (int i = 0; i < herd; i++) {
                    const glm::vec3 at(x + 0.5f + (i % 2) * 1.5f, static_cast<float>(y), z + 0.5f + (i / 2) * 1.5f);
                    if (farFromPlayers(at) && Spawn(wolves ? CreatureType::Wolf : CreatureType::Deer, at)) animals++;
                }
                break;
            }
        }

        // Ghouls in the dark: a few tries at a dark floor within 40 blocks (more, for
        // more of them, on a horde night).
        const int maxHostiles = horde ? 3 * kMaxHostilesNear : kMaxHostilesNear;
        for (int attempt = 0; attempt < (horde ? 9 : 3) && hostiles < maxHostiles; attempt++) {
            const int x = static_cast<int>(std::floor(player.feet.x + (unit(rng) * 2.0f - 1.0f) * 40.0f));
            const int z = static_cast<int>(std::floor(player.feet.z + (unit(rng) * 2.0f - 1.0f) * 40.0f));
            const int start = static_cast<int>(std::floor(player.feet.y)) + static_cast<int>(unit(rng) * 32.0f) - 20;
            for (int y = start; y > start - 16; y--) {
                bool grass = false;
                if (!CanStand(world, { x, y, z }, 2, grass)) continue;
                uint8_t block = 0, sky = 0;
                if (!LightAt(world, { x, y, z }, block, sky)) break;
                const float light = std::max(static_cast<float>(block), sky * (daylight > 0.5f ? 1.0f : 0.25f));
                const glm::vec3 at(x + 0.5f, static_cast<float>(y), z + 0.5f);
                if (light <= 4.0f && farFromPlayers(at)) {
                    // Underground (no sky at all), now and then bats instead; three in
                    // ten of the rest are skeletons.
                    if (sky == 0 && unit(rng) < 0.3f) Spawn(CreatureType::Bat, at + glm::vec3(0.0f, 1.0f, 0.0f));
                    else Spawn(unit(rng) < 0.3f ? CreatureType::Skeleton : CreatureType::Ghoul, at);
                    hostiles++;
                }
                break;
            }
        }
    }
}

void CreatureSystem::Update(ServerWorld& world, const std::vector<Player>& players, double now, float dt, float daylight, const HurtPlayer& hurt) {
    clock = now;
    if (naturalSpawning && !players.empty() && now >= nextSpawnCheck) {
        nextSpawnCheck = now + 1.0;
        SpawnNaturally(world, players, daylight);
    }

    for (auto it = creatures.begin(); it != creatures.end();) {
        Creature& c = *it;
        // Gone once no player is anywhere near (with nobody about, as they were: a
        // player who's just joined hasn't said where they are yet).
        float nearest = players.empty() ? 0.0f : 1e9f;
        for (const auto& p : players) nearest = std::min(nearest, glm::distance(p.feet, c.feet));
        // (A tame wolf stays while its owner's here: it catches up.)
        if (c.owner != 0)
            for (const auto& p : players)
                if (p.id == c.owner) nearest = 0.0f;
        if (nearest > kDespawnDistance || c.feet.y < -1000.0f) {
            it = creatures.erase(it);
            continue;
        }
        glm::vec2 move(0.0f);
        float speed = 0.0f;
        Think(c, players, now, hurt, move, speed);
        Move(world, c, move, speed, dt);
        // Ghouls burn in daylight.
        if (now >= c.nextBurn && Burning(world, c, daylight)) {
            c.nextBurn = now + 1.0;
            c.health -= 2;
            c.hurtUntil = now + 0.3;
        }
        if (c.health <= 0) it = creatures.erase(it);
        else ++it;
    }
}
