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
#include "world/Crafting.h"
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
    // dir: the side chiselled from, for a full block (Block::ShapeDir; it becomes that way up).
    // step: how far the corner moves, in eighths (1, 2 or 4: Block::kChiselSteps). Needs the chisel.
    void Chisel(const glm::ivec3& pos, int edge, bool raise, int dir = 0, int step = 2);
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
    // Machines' descriptions, as the server answered Inspect (newest last), kept apart from
    // the chat.
    const std::deque<ChatLine>& GetInspections() const { return inspections; }
    std::vector<uint16_t> tradePrices;
    // The dwarf sites around us, as the server last said (for the map).
    std::vector<net::SiteInfo> sites;
    std::optional<glm::ivec3> bed; // ours, if we have one
    // A line in this player's chat only (not sent anywhere).
    void Note(const std::string& text, net::ChatMessage::Kind kind = net::ChatMessage::Kind::Info) {
        chat.push_back({ kind, text, Now() });
        while (chat.size() > kMaxChatLines) chat.pop_front();
    }
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
    // When we last died (and respawned), and the name we joined with.
    double GetLastDeath() const { return lastDeath; }
    const std::string& GetMyName() const { return myName; }
    // Breath (survival): air left (0..GetMaxAir()) and what's taking it.
    int GetAir() const { return air; }
    // Hunger (survival): food points (0..GetMaxFood(), world/Hunger.h).
    int GetFood() const { return food; }
    int GetMaxFood() const { return maxFood; }
    int GetMaxAir() const { return maxAir; }
    Breathing::Cause GetAirCause() const { return airCause; }
    // An oxygen tank's fill (0..1), and whether we're breathing from it.
    float GetTank() const { return tank; }
    bool OnTank() const { return onTank; }
    float GetRain() const { return rain; }
    bool IsStorm() const { return storm; }
    // Lightning strikes since the last call.
    std::vector<glm::ivec3> TakeLightning() { std::vector<glm::ivec3> out; out.swap(lightning); return out; }
    // Explosions since the last call (where, and how big).
    std::vector<net::Explosion> TakeExplosions() { std::vector<net::Explosion> out; out.swap(explosions); return out; }
    // Game mode, and what we carry (world/Inventory.h: predicted on edits, put right by
    // the server).
    net::GameMode GetMode() const { return mode; }
    bool IsCreative() const { return mode == net::GameMode::Creative; }
    const Inventory& GetInventory() const { return inventory; }
    // Whether the server has said what we carry yet (since joining).
    bool HasInventory() const { return inventoryKnown; }
    // Units of a type we carry, whole and offcut (by what the type counts as: pebbles
    // show the stone held).
    uint32_t GetResources(BlockType type) const { return inventory.Units(ResourceTypeOf(type)); }
    // Whole items of a kind we carry.
    uint32_t GetCount(ItemId item) const { return inventory.Count(item); }
    // Armour worn: what's in our armour slots (world/Armour.h).
    bool HasArmour(ArmourPiece piece) const { return (armour & Armour::Bit(piece)) != 0; }
    uint8_t GetArmour() const { return armour; }
    // Tools: whether we carry one (creative: we have them all).
    bool HasTool(Tool tool) const { return tool == Tool::None || IsCreative() || inventory.BestTier(tool) >= 0; }
    // What the best one we carry is made of (0 stone, 1 iron, 2 steel; -1 none).
    int ToolTier(Tool tool) const { return IsCreative() ? Items::kTiers - 1 : inventory.BestTier(tool); }
    // The tool we really work with: that one if we have it, a hand if not.
    Tool Usable(Tool tool) const { return HasTool(tool) ? tool : Tool::None; }
    // Survival: make `count` of recipe `index` (Crafting::Recipes()); the server checks.
    // An upgrade works on `held`, the tool in our hand.
    void Craft(int index, int count, Tool held = Tool::None);
    // Whether this player could craft `count` of recipe `index` now (an upgrade: with
    // `held` in hand, of the tier below).
    bool CanCraft(int index, int count = 1, Tool held = Tool::None) const;
    // Moving items (net::InventoryMove): `count` (0: all) from one slot to another, or
    // to net::kAnySlot in a box. Our armour slots are numbered by piece.
    void MoveItems(net::Box fromBox, int fromSlot, net::Box toBox, int toSlot, int count = 0);
    // Creative: put `count` of an item in one of our slots (kNone empties it).
    void SetCreativeSlot(int slot, ItemId item, int count);
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
        uint8_t action = 0; // 1 attacking, 2 working (net::kCreatureAttacking, kCreatureWorking)
        float health = 1.0f; // what it has left (0..1)
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
    void Trade(uint16_t trader, uint8_t offer);
    // A trader's prices as the server last said (empty: not told yet).
    const std::vector<uint16_t>& GetTradePrices() const { return tradePrices; }
    // Right-clicked part of an air network: the server answers in chat.
    void Inspect(const glm::ivec3& pos);
    // Signal blocks' channels and states, as the server last said (world/Signals.h),
    // and setting a block's.
    const std::unordered_map<glm::ivec3, net::SignalInfo, Coords::IVec3Hash>& GetSignals() const { return signalInfos; }
    void ConfigureSignal(const net::SignalInfo& info);
    // The water wheels and machines near us, as of the server's last word.
    const std::vector<net::MachineInfo>& GetMachines() const { return machines; }
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
    // Up to `count` whole items of a kind, wherever they are, into the chest or out of it.
    void MoveChestItems(ItemId item, uint32_t count, bool intoChest);
    void MoveChestItems(BlockType type, uint32_t count, bool intoChest) { MoveChestItems(Items::OfBlock(type), count, intoChest); }
    // The chest open now (none until the server says it's open, or once it's closed),
    // and what's in it.
    std::optional<glm::ivec3> GetOpenChest() const { return openChest; }
    const Inventory& GetChest() const { return chestHeld; }
    uint32_t GetChestUnits(BlockType type) const { return chestHeld.Units(type); }

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
    std::vector<net::Explosion> explosions;
    std::vector<net::ArrowInfo> arrows;
    std::vector<net::MachineInfo> machines;
    std::unordered_map<glm::ivec3, net::SignalInfo, Coords::IVec3Hash> signalInfos;
    std::optional<glm::ivec3> bobber;
    bool bite = false;
    std::unordered_map<glm::ivec3, std::string, Coords::IVec3Hash> signs;
    double arrowsAt = 0.0;
    std::optional<glm::ivec3> openChest;
    Inventory chestHeld{ 0 };
    std::optional<glm::vec3> pendingCorrection;
    double lastStateSent = 0.0;

    struct Sample {
        double time;
        RemotePlayer state;
    };
    std::unordered_map<uint16_t, std::deque<Sample>> remote;
    std::unordered_map<uint16_t, std::string> playerNames; // by id (PlayerName)
    double lastDeath = -1000.0;
    std::string myName;
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
    float tank = 1.0f;
    bool onTank = false;
    net::GameMode mode = net::GameMode::Creative;
    Inventory inventory{ Inventory::kPlayerSlots };
    bool inventoryKnown = false;
    uint8_t armour = 0;
    double lastHurt = -1000.0;
    std::deque<ChatLine> chat; // the last kMaxChatLines
    static constexpr size_t kMaxChatLines = 100;
    std::deque<ChatLine> inspections; // the last kMaxInspections
    static constexpr size_t kMaxInspections = 32;

    std::unique_ptr<net::ChunkBlocks> blocks;
    std::unique_ptr<net::ChunkLight> light;
    Stats stats;
};
