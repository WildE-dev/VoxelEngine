#pragma once
// The server's creatures (kinds in world/Creatures.h): where they are, how they move
// and think, natural spawning around players, and fights with players. Creatures
// aren't saved: they come and go with the players around them.
//
// Movement is simple box physics against full blocks (standing on slopes like
// players do), with a hop up single-block steps. Deer wander and flee when hit;
// ghouls chase the nearest survival player in sight and hit them in reach, and burn
// in daylight. Natural spawning (normal worlds): herds of deer on grass around
// players, ghouls in dark places (light 4 or less, sky light counting for a quarter
// at night).
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

#include <glm/glm.hpp>

#include "world/Creatures.h"

class ServerWorld;

class CreatureSystem
{
public:
    struct Creature {
        uint16_t id = 0;
        CreatureType type = CreatureType::Deer;
        glm::vec3 feet{ 0.0f }, velocity{ 0.0f };
        float yaw = 0.0f; // degrees, 0 along +x, 90 along +z
        int health = 1;
        bool onGround = false;
        double hurtUntil = 0.0;   // flashes red until then
        double fleeUntil = 0.0;   // deer: running from fleeFrom
        glm::vec3 fleeFrom{ 0.0f };
        glm::vec2 wander{ 0.0f }; // current wandering direction (zero: standing)
        double nextDecision = 0.0;
        double nextAttack = 0.0;
        double nextBurn = 0.0;
    };

    // A player as the creatures see them.
    struct Player {
        uint16_t id = 0;
        glm::vec3 feet{ 0.0f };
        bool vulnerable = false; // survival and not in no-clip: ghouls go for them
    };
    // A ghoul hit a player for `damage` half hearts.
    using HurtPlayer = std::function<void(uint16_t playerId, int damage, const char* deathMessage)>;

    static constexpr size_t kMaxCreatures = 80;
    static constexpr float kDespawnDistance = 96.0f;

    explicit CreatureSystem(uint32_t seed = 0x5EEDC0DEu) : rng(seed) {}

    // Natural spawning on or off (the server turns it off for empty worlds).
    void SetNaturalSpawning(bool on) { naturalSpawning = on; }

    // Adds one; returns its id (0 if there are too many).
    uint16_t Spawn(CreatureType type, const glm::vec3& feet);
    // Runs everything for dt seconds. `now` is the server clock, `daylight` 0..1.
    void Update(ServerWorld& world, const std::vector<Player>& players, double now, float dt, float daylight, const HurtPlayer& hurt);

    // A player at `eye` hits creature `id` with `tool`. Returns whether it landed
    // (exists, in reach); `killed` says if it died.
    bool Attack(uint16_t id, const glm::vec3& eye, Tool tool, bool iron, double now, bool& killed);

    const std::vector<Creature>& All() const { return creatures; }
    const Creature* Find(uint16_t id) const;
    size_t Count() const { return creatures.size(); }
    void Clear() { creatures.clear(); }

private:
    void Think(Creature& c, const std::vector<Player>& players, double now, const HurtPlayer& hurt, glm::vec2& move, float& speed);
    void Move(ServerWorld& world, Creature& c, const glm::vec2& move, float speed, float dt);
    void SpawnNaturally(ServerWorld& world, const std::vector<Player>& players, float daylight);
    bool Burning(ServerWorld& world, const Creature& c, float daylight) const;

    std::vector<Creature> creatures;
    uint16_t nextId = 1;
    std::mt19937 rng;
    bool naturalSpawning = true;
    double nextSpawnCheck = 0.0;
    double clock = 0.0;
};
