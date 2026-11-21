#pragma once
// The server's creatures (kinds in world/Creatures.h): where they are, how they move
// and think, natural spawning around players, and fights with players. Creatures
// aren't saved: they come and go with the players around them.
//
// Movement is simple box physics against full blocks (standing on slopes like
// players do), with a hop up single-block steps. Deer wander and flee when hit.
// Hostile dwarves keep near the camp or mine they live in (their home), go for the
// nearest survival player in sight and hit them in reach (crossbowmen keep their
// distance and shoot), and give up on anyone who leads them too far from home. Natural
// spawning (normal worlds): herds of deer (or a pair of wolves) on grass around
// players, bats in the dark of caves, and dwarves at the camps and mines near
// players (world/Dwarves.h): a site fills up when a player first comes near, then
// gets one back every kSiteRefill seconds. On a horde night a war band of dwarves
// (no home) comes for players from all round.
#include <cstdint>
#include <functional>
#include <random>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "world/Collision.h"
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
        // Wolves: their owner (0: wild), and the player they're angry with until when.
        uint16_t owner = 0;
        uint16_t angryAt = 0;
        double angryUntil = 0.0;
        // Dwarves: where they live (a site's middle, or a miner's post), if anywhere.
        bool hasHome = false;
        glm::vec3 home{ 0.0f };
        glm::ivec3 site{ 0 };
        // What it last did (1 attacked, 2 worked) and when: the clients animate it.
        uint8_t act = 0;
        double actedAt = -1e9;
    };
    // How long a creature counts as attacking or working after it did, for the clients.
    static constexpr double kAttackShown = 0.6, kWorkShown = 0.9;
    // A creature did something (1 attacked, 2 worked).
    void Act(uint16_t id, uint8_t what, double now);

    // A player as the creatures see them.
    struct Player {
        uint16_t id = 0;
        glm::vec3 feet{ 0.0f };
        bool vulnerable = false; // survival and not in no-clip: hostile dwarves go for them
    };
    // A dwarf (or a wolf) hit a player for `damage` half hearts.
    using HurtPlayer = std::function<void(uint16_t playerId, int damage, const char* deathMessage)>;
    // Crossbowmen shoot: a bolt (an arrow) from here, this way (the server flies it).
    using ShootArrow = std::function<void(const glm::vec3& from, const glm::vec3& direction)>;
    void SetShooter(ShootArrow shoot) { shooter = std::move(shoot); }

    static constexpr size_t kMaxCreatures = 80;
    static constexpr float kDespawnDistance = 96.0f;

    explicit CreatureSystem(uint32_t seed = 0x5EEDC0DEu) : rng(seed) {}

    // Natural spawning on or off (the server turns it off for empty worlds).
    void SetNaturalSpawning(bool on) { naturalSpawning = on; }
    bool NaturalSpawning() const { return naturalSpawning; }
    // A horde night: three times the hostile creatures around each player, spawning
    // faster, and hunting from twice as far.
    void SetHorde(bool on) { horde = on; }
    bool Horde() const { return horde; }

    // Adds one; returns its id (0 if there are too many).
    uint16_t Spawn(CreatureType type, const glm::vec3& feet);
    // A dwarf miner who works the mining post at `post` (keeps near it).
    uint16_t SpawnMiner(const glm::vec3& feet, const glm::ivec3& post);
    void Remove(uint16_t id);
    static constexpr double kSiteRefill = 30.0;
    // Runs everything for dt seconds. `now` is the server clock, `daylight` 0..1.
    void Update(ServerWorld& world, const std::vector<Player>& players, double now, float dt, float daylight, const HurtPlayer& hurt);

    // A player at `eye` hits creature `id` with `tool`. Returns whether it landed
    // (exists, in reach); `killed` says if it died.
    bool Attack(uint16_t id, const glm::vec3& eye, Tool tool, int tier, double now, bool& killed);
    // Hurts it (from anywhere: an arrow), knocking it back away from `from`.
    bool Damage(uint16_t id, int damage, const glm::vec3& from, double now, bool& killed);
    // A player hit it: a wild wolf turns on them for a while.
    void Provoke(uint16_t id, uint16_t player, double now);
    // Tames a wild wolf for this player (false if it isn't one, or already has an owner).
    bool Tame(uint16_t id, uint16_t player);
    // Its box, for hit tests.
    static Collision::AABB Box(const Creature& c);

    const std::vector<Creature>& All() const { return creatures; }
    const Creature* Find(uint16_t id) const;
    size_t Count() const { return creatures.size(); }
    void Clear() { creatures.clear(); }

private:
    void Think(Creature& c, const std::vector<Player>& players, double now, const HurtPlayer& hurt, glm::vec2& move, float& speed);
    void Move(ServerWorld& world, Creature& c, const glm::vec2& move, float speed, float dt);
    void SpawnNaturally(ServerWorld& world, const std::vector<Player>& players, float daylight);
    // Dwarves at the sites near players, and war bands on horde nights.
    void SpawnDwarves(ServerWorld& world, const std::vector<Player>& players);
    std::unordered_map<int64_t, double> siteRefill; // when each site gets its next dwarf back

    std::vector<Creature> creatures;
    uint16_t nextId = 1;
    std::mt19937 rng;
    bool naturalSpawning = true;
    bool horde = false;
    double nextSpawnCheck = 0.0;
    double clock = 0.0;
    ShootArrow shooter;
};
