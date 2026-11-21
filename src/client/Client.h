#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "net/Protocol.h"
#include "world/Armour.h"
#include "world/Coords.h"
#include "world/Breathing.h"
#include "net/Transport.h"

class ClientWorld;

// The client's side of the connection: applies what the server sends to a ClientWorld,
// sends our player state and edit requests, and keeps other players' recent states for
// interpolation. Main thread only.
//
// Our own edits are predicted: they're applied to the local world at once and sent to
// the server, which answers with the authoritative block (the change itself, or the
// old block again if it refused the edit).
class Client
{
public:
    struct RemotePlayer {
        uint16_t id = 0;
        glm::vec3 feet{0.0f};
        float yaw = 0.0f, pitch = 0.0f;
        uint8_t flags = 0;
        Tool heldTool = Tool::None;          // what's in their hand
        BlockType heldBlock = BlockType::AIR;
        uint8_t armour = 0;                  // armour worn (Armour::Bit each)
        std::string name;                    // (empty until the server says)
    };

    explicit Client(ClientWorld& world);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Connects and sends Hello (blocks up to a few seconds). The server's Welcome
    // arrives through Poll.
    // viewDistance: chunks we'd like to see (0: as far as the server sends).
    bool Connect(const std::string& host, uint16_t port, const std::string& name, std::string& error, int viewDistance = 0);
    void Disconnect();

    // Handles everything that has arrived. Call once per frame.
    void Poll();

    bool IsConnected() const { return net.IsConnected(); }
    bool IsWelcomed() const { return welcomed; }
    // Why the connection ended (empty while connected).
    const std::string& GetDisconnectReason() const { return disconnectReason; }
    uint16_t GetPlayerId() const { return playerId; }
    glm::vec3 GetSpawn() const { return spawn; }
    // The Welcome's flags (net::kWelcomeResume, net::kFlagFlying) and look direction.
    uint8_t GetSpawnFlags() const { return spawnFlags; }
    float GetSpawnYaw() const { return spawnYaw; }
    float GetSpawnPitch() const { return spawnPitch; }

    // A position correction from the server, once.
    std::optional<glm::vec3> TakeCorrection();
    // Pushes from being hit since the last call (add to the player's velocity).
    glm::vec3 TakeKnockback() { const glm::vec3 k = pendingKnockback; pendingKnockback = glm::vec3(0.0f); return k; }

    // Sent at most kStateRate times a second; extra calls are ignored.
    // `heldTool` / `heldBlock`: what's in hand, for others to see.
    void SendPlayerState(const glm::vec3& feet, const glm::vec3& velocity, float yaw, float pitch, uint8_t flags, Tool heldTool = Tool::None,
                         BlockType heldBlock = BlockType::AIR);

    // Edits (predicted locally).
    // In survival, breaking takes time (world/Mining.h): StartMining when starting on a
    // block, BreakBlock once it's been long enough, with the same tool.
    void StartMining(const glm::ivec3& pos, Tool tool);
    void BreakBlock(const glm::ivec3& pos, Tool tool = Tool::None);
    // heightEighths: how tall a shapeable block is placed (8 full, see Block::Placed).
    // faceAxis: the axis of the face it's placed against (0 x, 1 y, 2 z); logs lie along it.
    void PlaceBlock(const glm::ivec3& pos, BlockType type, int heightEighths = 8, int faceAxis = 1);
    // Lowers (or raises) one quarter of a block by a quarter (predicted, like edits).
    void Chisel(const glm::ivec3& pos, int edge, bool raise);
    void Carve(const std::vector<glm::ivec3>& centers, int radius);
    // Asks the server to skip to the next dawn or dusk.
    void SkipTime();

    // Chat: send a line (a command if it starts with '/'), and the lines received.
    struct ChatLine {
        net::ChatMessage::Kind kind;
        std::string text;
        double time; // when it arrived (seconds, same clock as the snapshots)
    };
    void SendChat(const std::string& text);
    const std::deque<ChatLine>& GetChat() const { return chat; }
    static double Now();

    // The sky, from the latest snapshot: time of day (0..1, 0 sunrise, 0.25 noon,
    // 0.5 sunset, 0.75 midnight) and rain intensity (0..1).
    float GetTimeOfDay() const { return timeOfDay; }
    // What the server lets us do (net::kAbility*).
    uint8_t GetAbilities() const { return abilities; }
    // Health in half hearts, and when we were last hurt (Now() clock; very negative if never).
    int GetHealth() const { return health; }
    int GetMaxHealth() const { return maxHealth; }
    double GetLastHurt() const { return lastHurt; }
    // Breath (survival): air left (0..GetMaxAir()) and what's taking it.
    int GetAir() const { return air; }
    // Hunger (survival): food points (0..GetMaxFood(), world/Hunger.h).
    int GetFood() const { return food; }
    int GetMaxFood() const { return maxFood; }
    int GetMaxAir() const { return maxAir; }
    Breathing::Cause GetAirCause() const { return airCause; }
    float GetRain() const { return rain; }
    bool IsStorm() const { return storm; }
    // Lightning strikes since the last call.
    std::vector<glm::ivec3> TakeLightning() { std::vector<glm::ivec3> out; out.swap(lightning); return out; }
    // Game mode, and in survival the resource units held of each block type (predicted
    // on edits, put right by the server).
    net::GameMode GetMode() const { return mode; }
    bool IsCreative() const { return mode == net::GameMode::Creative; }
    // (By what the type counts as: pebbles show the stone held.)
    uint32_t GetResources(BlockType type) const { return resources[static_cast<size_t>(ResourceTypeOf(type))]; }
    // Tools: whether we have one (creative: all of them; survival: those we've made).
    // Armour worn (survival; world/Armour.h).
    bool HasArmour(ArmourPiece piece) const { return (armour & Armour::Bit(piece)) != 0; }
    // The iron upgrade (Crafting::kIronBit): faster tools, harder hits.
    bool HasIron() const { return (tools & (1u << 7)) != 0; }
    uint8_t GetArmour() const { return armour; }
    bool HasTool(Tool tool) const { return tool == Tool::None || IsCreative() || (tools & (1u << static_cast<unsigned>(tool))); }
    // The tool we really work with: that one if we have it, a hand if not.
    Tool Usable(Tool tool) const { return HasTool(tool) ? tool : Tool::None; }
    // Survival: make `count` of recipe `index` (Crafting::Recipes()); the server checks.
    void Craft(int index, int count);
    // Whether this player could craft `count` of recipe `index` now (and hasn't the tool yet).
    bool CanCraft(int index, int count = 1) const;
    // Whether we may place this (creative, or we hold enough of it).
    bool CanAfford(BlockType type, int heightEighths) const;

    // Other players, interpolated kInterpolationDelay seconds in the past so movement is
    // smooth between snapshots.
    std::vector<RemotePlayer> GetRemotePlayers() const;

    // Creatures near us, smoothed like other players. `moving`: walking speed now
    // (blocks per second), for the legs.
    struct Creature {
        uint16_t id = 0;
        CreatureType type = CreatureType::Deer;
        glm::vec3 feet{0.0f};
        float yaw = 0.0f;
        bool hurt = false;
        float speed = 0.0f;
        bool tame = false; // a wolf with an owner
    };
    std::vector<Creature> GetCreatures() const;

    // Blocks someone else broke or placed (as the server's changes arrive; our own,
    // predicted edits don't show up here again), for sounds. Taken and cleared.
    struct BlockEvent {
        glm::ivec3 pos{0};
        BlockType type = BlockType::AIR; // what was broken, or placed
        bool placed = false;
    };
    std::vector<BlockEvent> TakeBlockEvents();
    // Hit a creature (the server checks the reach).
    void Attack(uint16_t creature, Tool tool);
    void AttackPlayer(uint16_t player, Tool tool);
    // Right-clicked a creature (with raw venison in hand, a wild wolf is tamed).
    void Interact(uint16_t creature);
    // Eat one of a food item held (survival; the server checks it).
    void Eat(BlockType item);
    // Chests: ask to open the one at `pos` (the server answers with its contents), close
    // it, and move units of a type into or out of it (the server's answer updates both).
    void OpenChest(const glm::ivec3& pos);
    // Right-clicked a bed: respawn there (and sleep, at night). The server says so in chat.
    void UseBed(const glm::ivec3& pos);
    // Shoot an arrow from the bow, this way (the server checks the bow and the arrow).
    void Shoot(const glm::vec3& direction);
    // Fishing: cast into the water at `pos`, or reel in.
    void Cast(const glm::ivec3& pos, bool cast);
    // Signs: write on one, and what's written on every sign we know of.
    void WriteSign(const glm::ivec3& pos, const std::string& text);
    const std::unordered_map<glm::ivec3, std::string, Coords::IVec3Hash>& GetSigns() const { return signs; }
    // Where our bobber floats (none if it's in), and whether a fish bit since the last call.
    std::optional<glm::ivec3> GetBobber() const { return bobber; }
    bool TakeBite() { const bool b = bite; bite = false; return b; }
    // Arrows near us, as of the last update, moved on by their speed since.
    std::vector<net::ArrowInfo> GetArrows() const;
    void CloseChest();
    void MoveChestItems(BlockType type, uint32_t units, bool intoChest);
    // The chest open now (none until the server says it's open, or once it's closed),
    // and what's in it.
    std::optional<glm::ivec3> GetOpenChest() const { return openChest; }
    uint32_t GetChestUnits(BlockType type) const { return chestHeld[static_cast<size_t>(type)]; }

    struct Stats {
        uint32_t roundTripMs = 0;
        uint64_t bytesReceived = 0;
        uint32_t chunksReceived = 0;
        uint32_t deltasReceived = 0; // cells
        uint32_t lightUpdates = 0;
    };
    Stats GetStats() const;

    static constexpr int kStateRate = 20;
    static constexpr double kInterpolationDelay = 0.1;

private:
    void HandleMessage(const std::vector<uint8_t>& data);

    ClientWorld& world;
    net::NetClient net;
    bool welcomed = false;
    std::string disconnectReason;
    uint16_t playerId = 0;
    glm::vec3 spawn{0.0f};
    uint8_t spawnFlags = 0;
    float spawnYaw = -90.0f, spawnPitch = 0.0f;

    uint32_t stateSequence = 0;
    uint16_t correctionId = 0;
    glm::vec3 pendingKnockback{ 0.0f };
    bool storm = false;
    std::vector<glm::ivec3> lightning;
    std::vector<net::ArrowInfo> arrows;
    std::optional<glm::ivec3> bobber;
    bool bite = false;
    std::unordered_map<glm::ivec3, std::string, Coords::IVec3Hash> signs;
    double arrowsAt = 0.0;
    std::optional<glm::ivec3> openChest;
    std::array<uint32_t, static_cast<size_t>(BlockType::COUNT)> chestHeld{};
    std::optional<glm::vec3> pendingCorrection;
    double lastStateSent = 0.0;

    struct Sample {
        double time;
        RemotePlayer state;
    };
    std::unordered_map<uint16_t, std::deque<Sample>> remote;
    std::unordered_map<uint16_t, std::string> playerNames; // by id (PlayerName)
    struct CreatureSample {
        double time;
        Creature state;
    };
    std::unordered_map<uint16_t, std::deque<CreatureSample>> creatures;
    uint32_t lastCreatureTick = 0;
    std::vector<BlockEvent> blockEvents;
    uint32_t lastSnapshotTick = 0;
    float timeOfDay = 0.05f, rain = 0.0f;
    uint8_t abilities = 0;
    int health = 20, maxHealth = 20;
    int air = 20, maxAir = 20;
    int food = 20, maxFood = 20;
    Breathing::Cause airCause = Breathing::Cause::None;
    net::GameMode mode = net::GameMode::Creative;
    std::array<uint32_t, static_cast<size_t>(BlockType::COUNT)> resources{};
    uint8_t tools = 0;
    uint8_t armour = 0;
    double lastHurt = -1000.0;
    std::deque<ChatLine> chat; // the last kMaxChatLines
    static constexpr size_t kMaxChatLines = 100;

    std::unique_ptr<net::ChunkBlocks> blocks;
    std::unique_ptr<net::ChunkLight> light;
    Stats stats;
};
