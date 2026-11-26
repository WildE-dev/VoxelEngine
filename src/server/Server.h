#pragma once

#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include <cmath>

#include "net/Protocol.h"
#include "net/Transport.h"
#include "server/Creatures.h"
#include "server/DroppedItems.h"
#include "server/Steam.h"
#include "world/Breathing.h"
#include "world/Achievements.h"
#include "world/Hunger.h"
#include "world/Power.h"
#include "world/Signals.h"
#include "world/Crafting.h"
#include "world/Coords.h"
#include "world/Inventory.h"
#include "world/TerrainGenerator.h"

class ServerWorld;
class WorldStorage;

// The authoritative game server: owns the world (generation, edits, water, light) and
// streams it to connected clients over ENet. Runs headless; nothing here touches GL.
//
// Everything happens on the server thread (Run): network events are handled as they
// arrive and the world is streamed out on a fixed tick (kTickRate). For each client the
// server tracks which chunks it has sent, and only ever sends deltas for those:
//  - New chunks go out nearest first once generated and lit, as RLE chunk data.
//  - Block changes (player edits and flowing water) are coalesced per client and per
//    chunk until sent, so a cell that changes several times between sends goes out
//    once with its latest value. They're sent nearest chunk first within a per-tick
//    byte budget; whatever doesn't fit waits for the next tick. A chunk with very many
//    pending cells is re-sent whole instead.
//  - Chunks whose light changed get their new light, coalesced the same way.
//  - Chunks that leave a client's view distance are unloaded on that client.
//
// With a world directory, the world is saved there (see WorldStorage): edited chunks,
// and each player's position by name, so rejoining puts them back where they left.
// It autosaves every `autosaveSeconds` and saves when the server stops.
//
// The server also runs the clock and the weather: a day lasts `dayLengthSeconds`, and
// rain comes now and then (clear spells of 10-25 minutes, showers of 2-5), fading in
// and out over ten seconds. Both go to every client in the player snapshot; rain also
// tops up bodies of water (WaterSimulation::SetRaining).
//
// Chat: a player's line goes to everyone; a line starting with '/' is a command (see
// ServerCommands.cpp; /help lists them). Commands can also be typed into a dedicated
// server's console (QueueConsoleCommand).
//
// Game modes: in creative, players build with anything and nothing hurts them; in
// survival, every block costs its worth (from the player's inventory, world/Inventory.h)
// that players collect by breaking blocks, and traps hurt. The server keeps everyone's
// mode and inventory (saved with the player) and refuses what they can't afford.
//
// Crafting (world/Crafting.h): survival players turn what they have into other blocks,
// tools and armour, which are items in their inventory like any other.
//
// Creatures (server/Creatures.h): deer, wolves, bats and dwarves, run here and sent to the players
// near them every tick; players hit them with Attack.
//
// Mining (HandleMineStart, HandleBreak): in survival breaking a block takes time
// (world/Mining.h). The client says when it starts on a block, and the break only
// goes through once that long has passed.
//
// Health and traps (UpdateTraps): players have kMaxHealth half hearts. Standing in
// spikes hurts; crumbling bricks give way soon after someone stands on them (and the
// collapse spreads to crumbling bricks touching them); a pressure plate sends on its
// channel (world/Signals.h) while it's stood on. At no health a player respawns at the spawn point.
// Health comes back slowly once nothing has hurt them for a while. Players in no-clip
// set off nothing and can't be hurt. Players also need air (UpdateBreath): it runs
// out under water, and slowly far up (above 300) and deep underground (below -100).
//
// Permissions: players can chat and use the harmless commands; operators can also
// change the world's time and weather, teleport, save, kick and make or unmake other
// operators. The console is always an operator, and so is `ownerName` (singleplayer's
// host). The operators are kept in ops.txt in the world directory, one name per line
// (case doesn't matter). Players are only known by the name they join with, so this
// stops casual misuse, not someone joining under an operator's name.
class Server
{
public:
    static constexpr int kTickRate = 20;
    static constexpr size_t kMaxClients = 16;
    // A connection has this long to say Hello, or it's dropped: silent ones would
    // otherwise hold the few places there are for as long as they liked.
    static constexpr double kHelloSeconds = 10.0;

    static constexpr int kMaxHealth = 20;           // half hearts
    static constexpr int kSpikeDamage = 4;          // two hearts...
    static constexpr double kSpikeInterval = 0.5;   // ...every half second
    static constexpr double kCrumbleDelay = 0.4;    // after first being stood on
    static constexpr double kCrumbleSpread = 0.12;  // to each crumbling brick touching it
    static constexpr double kRegenDelay = 5.0, kRegenInterval = 4.0; // a heart per 4 s, 5 s after harm

    struct Options {
        std::string bindHost = "0.0.0.0";
        uint16_t port = net::kDefaultPort;
        bool log = true;
        // Where the world is saved; empty: nothing is saved (tests).
        std::string worldDirectory;
        float autosaveSeconds = 60.0f;
        // Chunks around each player in which water flows (less than the view distance,
        // to save CPU); further out it pauses until a player comes back.
        int waterSimulationDistance = 6;
        // Generator for a new world (a saved world keeps the one it was created with).
        TerrainGenerator::Type worldType = TerrainGenerator::Type::Normal;
        float dayLengthSeconds = 20.0f * 60.0f;
        enum class Weather { Auto, Clear, Rain, Storm } weather = Weather::Auto;
        // Always an operator (singleplayer: the local player). Empty: nobody.
        std::string ownerName;
        // Made operators at startup, besides those in ops.txt (--op NAME).
        std::vector<std::string> operators;
        // The game mode new players start in, for a new world (a saved world keeps its
        // own; /gamemode changes a player's).
        net::GameMode defaultMode = net::GameMode::Survival;
        // Creatures spawn by themselves around players (normal worlds only; /summon
        // works anywhere).
        bool creatures = true;
        // Survival players who die leave what they carry (and wear) in a chest there.
        bool dropOnDeath = true;
        // One night in this many is a horde night (tests make it every night).
        int hordeOdds = 4;
        // How fast fish bite (1: normal, 4 to 14 seconds; tests speed it up).
        float fishingRate = 1.0f;
        // How fast crops ripen (1: normal, a minute and a half on average; tests speed it up).
        float cropGrowthRate = 1.0f;
        // How fast players get hungry (1: normal; tests speed it up).
        float hungerRate = 1.0f;
        // Players can hurt each other (operators switch it with /pvp).
        bool pvp = true;
        // Chunks loaded and sent around each player (2..12): less is lighter on the
        // machine. Players who ask for less get less.
        int viewDistance = 12;
    };

    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds the socket. Returns false (with a message on stderr) if it can't.
    bool Start(const Options& options);
    uint16_t GetPort() const;

    // Runs until Stop() is called (from any thread, or a signal handler), then saves.
    void Run();
    // Runs on a thread of its own (singleplayer's in-process server).
    void RunInBackground();
    void Stop();

    // A line typed into the server's console, run on the server thread at the next tick
    // as a command (with or without the leading '/'). Any thread.
    void QueueConsoleCommand(const std::string& line);

private:
    struct ClientState {
        net::PeerId peer = 0;
        uint16_t id = 0;
        std::string name;
        bool welcomed = false;
        bool kicked = false;

        glm::vec3 feet{0.0f};
        glm::vec3 lastValid{0.0f};
        double lastStateTime = 0.0;
        double lastHeard = 0.0; // when it last sent anything (a live game: many times a second)
        double connectedAt = 0.0; // (one that never says Hello is let go: kHelloSeconds)
        float yaw = 0.0f, pitch = 0.0f;
        uint8_t flags = 0;
        uint32_t lastSequence = 0;
        uint16_t correctionId = 0;
        int blockedStates = 0; // consecutive states inside solid blocks

        glm::ivec3 center{0};
        bool hasCenter = false;
        int viewDistance = 0, verticalViewDistance = 0; // chunks: what they asked for, up to ours (set on Hello)
        bool allSent = false; // every chunk in range has been sent (until the centre moves)

        bool hasState = false; // has sent a valid PlayerState since joining
        bool noclip = false;   // allowed to fly through blocks (/noclip)

        net::GameMode mode = net::GameMode::Survival;
        Inventory inventory{ Inventory::kPlayerSlots }; // what they carry (the hotbar first)
        uint8_t armour = 0; // armour worn: what's in their armour slots (Armour::Bit each)
        std::optional<glm::ivec3> openChest; // the chest they have open
        std::optional<glm::ivec3> bed;       // where they respawn (if it's still a bed)
        double sleptAt = -1000.0;            // when they last got into bed at night
        float fallPeak = 0.0f;               // highest point since last on the ground (UpdateFall)
        bool falling = false;
        glm::vec3 fallVelocity{ 0.0f };      // the velocity they last said while in the air
        Tool heldTool = Tool::None;          // in their hand, for others to see
        ItemId heldItem = Items::kNone;  

        int health = kMaxHealth;
        int sentHealth = -1;          // last health the client was told
        double lastHurt = -1000.0;    // when anything last hurt them
        double nextSpikeHit = 0.0;    // spikes hurt at most this often
        double nextRegen = 0.0;
        double invulnerableUntil = 0.0; // just respawned
        // Breath (UpdateBreath): air left, what's taking it, and what the client was told.
        float air = Breathing::kMaxAir;
        Breathing::Cause airCause = Breathing::Cause::None;
        int sentAir = -1;
        Breathing::Cause sentAirCause = Breathing::Cause::None;
        float tank = Breathing::kTankSeconds; // an oxygen tank's air (seconds), if they wear one
        bool onTank = false;
        int sentTank = -1; // as last sent: 0..255 (see TankByte)
        bool sentOnTank = false;
        bool sentBurning = false;
        double nextBreathHit = 0.0;
        // Hunger (UpdateHunger): food left, drain not yet taken, where they were last
        // tick (walking drains it), when starving next hurts, what the client was told.
        int food = Hunger::kMaxFood;
        float exhaustion = 0.0f;
        glm::vec3 hungerFeet{ 0.0f };
        bool hasHungerFeet = false;
        double nextStarve = 0.0;
        int sentFood = -1;
        // Mining (survival): the block they're breaking, with what, since when.
        bool mining = false;
        glm::ivec3 miningPos{ 0 };
        Tool miningTool = Tool::None;
        double miningSince = 0.0;
        double nextAttack = 0.0; // hitting creatures: at most this often
        double nextShot = 0.0;   // the bow: at most this often
        uint32_t achievements = 0; // earned (Achievements::Bit each)
        glm::ivec2 sitesRegion{ INT_MIN, INT_MIN }; // where they were when last told the sites round them
        size_t sitesFallen = 0;                     // (and how many kings had fallen then)
        bool sentArrows = false; // the last Arrows they were sent had some in it
        uint64_t sentItems = 0;  // what the last ItemsNear they were sent held (a hash; 0: nothing)
        int itemsQuiet = 0;      // ticks since then
        bool sentSteam = false;  // the last SteamNear they were sent had some in it
        double nextLavaBurn = 0.0; // standing in lava burns at most this often
        double nextScald = 0.0;    // thick steam scalds at most this often
        double burningUntil = 0.0; // alight (world/Fire.h) until then, unless they get into water
        double nextBurnHurt = 0.0;
        // Movement checks (HandlePlayerState): until when a knock (a hit, a blast) lets them
        // move faster; where and when they were last held up (on the ground, in water or
        // lava, on a ladder, in a boat), so survival players can't fly or hover; and the
        // mode those were last checked in.
        double knockedUntil = 0.0;
        double launchedUntil = 0.0; // thrown by a launcher: no air checks till then
        glm::vec3 support{ 0.0f }; // (where: corrections for flying put them back there)
        double supportAt = 0.0;
        bool checkedFree = true; // creative or no-clip last time (no air checks)
        bool sentMachines = false; // the last Machines they were sent had some in it
        // Fishing: where their bobber floats (if it's out), where they cast from, and
        // when a fish will bite.
        std::optional<glm::ivec3> bobber;
        glm::vec3 castFrom{ 0.0f };
        double biteAt = 0.0;

        // Chunks the client has, with the version of the light it has for each.
        std::unordered_map<glm::ivec3, uint32_t, Coords::IVec3Hash> sent;
        // Changes not sent yet, for chunks the client has: cells (chunk-local indices)
        // and chunks whose light changed.
        std::unordered_map<glm::ivec3, std::unordered_set<uint16_t>, Coords::IVec3Hash> pendingCells;
        std::unordered_set<glm::ivec3, Coords::IVec3Hash> pendingLight;
    };

    void Loop(); // Run's body: ticks until `running` is cleared, then saves
    void HandleEvent(const net::NetEvent& event);
    void HandleMessage(ClientState& client, const std::vector<uint8_t>& data);
    void HandleHello(ClientState& client, const net::Hello& hello);
    void HandlePlayerState(ClientState& client, const net::PlayerState& state);
    void HandleBreak(ClientState& client, const glm::ivec3& pos, Tool tool);
    void HandleMineStart(ClientState& client, const glm::ivec3& pos, Tool tool);
    void HandleAttack(ClientState& client, uint16_t creature, Tool tool);
    void HandleAttackPlayer(ClientState& client, uint16_t player, Tool tool);
    void HandleInteract(ClientState& client, uint16_t creature);
    void HandleTrade(ClientState& client, uint16_t trader, uint8_t offer);
    // A raid on a player's home (their bed): see CreatureSystem::StartRaid. One night in
    // kRaidOdds that isn't a horde night, for each survival player near their bed.
    bool StartRaid(ClientState& client);
    static constexpr uint32_t kRaidOdds = 4;
    // Trade prices (world/Trades.h): how far each offer's price is pushed up, eased back
    // a step every Trades::kPriceRecovery; and telling a player them.
    std::vector<int> tradePressure;
    double nextPriceRecovery = 0.0;
    void SendTradePrices(ClientState& client, uint16_t trader);
    // A Dwarf King fell to this player: his hoard, an achievement, and everyone told.
    void KingSlain(ClientState& client);
    void GolemSlain(ClientState& client);
    // The strongholds whose king has fallen, kept in the world's kings.txt.
    void LoadFallenKings();
    void SaveFallenKings();
    size_t fallenKingsSaved = 0;
    // `upgrade`: the tool an upgrade recipe works on (the one in their hand).
    void HandleCraft(ClientState& client, int recipe, int count, Tool upgrade);
    // The tool a player really works with: theirs if they carry one, a bare hand if not
    // (creative players have every tool, of steel).
    static Tool Usable(const ClientState& client, Tool tool);
    // What that tool of theirs is made of (the best they carry): 0 stone, 1 iron, 2 steel.
    static int ToolTier(const ClientState& client, Tool tool);
    // Moving items between their slots, their armour slots and the open chest.
    void HandleInventoryMove(ClientState& client, const net::InventoryMove& m);
    void HandleCreativeSlot(ClientState& client, const net::CreativeSlot& m);
    void HandleDropItem(ClientState& client, const net::DropItem& m);
    void HandleUseBucket(ClientState& client, const net::UseBucket& m);
    // Gives a player units of a type (survival); what doesn't fit is lost, and they're
    // told. Returns whether it all fitted. (The caller sends the inventory.)
    bool Give(ClientState& client, ItemRef item, uint32_t units);
    // A tick of the creatures, and telling each player about the ones near them.
    void UpdateCreatures();
    void SendCreatures();
    void HandlePlace(ClientState& client, const glm::ivec3& pos, BlockType type, int height, int faceAxis);
    void HandleChisel(ClientState& client, const net::EditChisel& chisel);
    // The armour worn, as a little inventory of a slot per BodySlot, and back.
    static Inventory ArmourBox(uint8_t worn);
    static uint8_t WornFrom(const Inventory& box);
    // A shovel's right-click: the ground `radius` round the block clicked (1: 3 by 3) levelled
    // to its surface (soft ground shaved off above, filled in below with what was clicked;
    // anything else left be). Survival: what's taken off is given back, what's filled costs.
    void HandleFlatten(ClientState& client, const glm::ivec3& pos, int radius);
    void HandleCarve(ClientState& client, const net::EditCarve& carve);
    void HandleChat(ClientState& client, const std::string& text);

    // Commands (ServerCommands.cpp). `sender` is null for the console; `reply` answers
    // whoever ran it.
    using Reply = std::function<void(net::ChatMessage::Kind, const std::string&)>;
    void RunCommand(const std::string& line, ClientState* sender, const Reply& reply);
    void RunConsoleCommands();
    // Sends a line to one client, or to everyone (and the log).
    void SendChat(ClientState& client, net::ChatMessage::Kind kind, const std::string& text);
    void Broadcast(net::ChatMessage::Kind kind, const std::string& text);
    ClientState* FindPlayer(const std::string& name);
    // Operators (names compared without case). `sender` null means the console.
    bool IsOperator(const ClientState* sender) const;
    bool IsOperatorName(const std::string& name) const;
    // A name as players are known by: nothing unprintable, no spaces round it.
    static std::string CleanName(const std::string& raw);
    // What killing a creature gives the player who did it (by hand or by arrow).
    void RewardKill(ClientState& client, CreatureType type, bool raider);
    // Adds or removes a name; returns false if nothing changed. Saves ops.txt.
    bool SetOperator(const std::string& name, bool op);
    void LoadOperators();
    void SaveOperators() const;
    // Moves a player (they're told with a correction, like a rejected move).
    void Teleport(ClientState& client, const glm::vec3& feet);
    void SetTimeOfDay(double t) { timeOfDay = t - std::floor(t); }
    // The time of day as it's sent and saved, a float. The last sliver of a day rounds
    // up to 1 as a float, which is out of range for both (a client drops that snapshot,
    // and a world saved just then would refuse to load): that's 0, the next sunrise.
    float TimeOfDayFloat() const {
        const float t = static_cast<float>(timeOfDay);
        return t < 1.0f ? t : 0.0f;
    }
    // Tells a client what it may do (after joining, /op, /deop, /noclip).
    void SendAbilities(ClientState& client);
    // Survival bookkeeping: tells the client its mode / what it holds.
    void SendMode(ClientState& client);
    void SendInventory(ClientState& client);
    static bool Creative(const ClientState& client) { return client.mode == net::GameMode::Creative; }
    // Just above the ground at the spawn column (respawning, /spawn): mountains may be
    // anywhere, so it's worked out from the terrain.
    glm::vec3 SpawnPoint() const;

    // Traps and health, every tick.
    void UpdateTraps();
    // A tick of a player's breathing: drains or refills their air, and hurts them
    // once it has run out (see world/Breathing.h).
    void UpdateBreath(ClientState& client, float dt);
    void UpdateHunger(ClientState& client, float dt);
    void HandleEat(ClientState& client, ItemId food);
    // Chests: what's in each (world/Inventory.h), by position; saved with the world.
    using ChestItems = Inventory;
    void HandleOpenChest(ClientState& client, const glm::ivec3& pos, bool open);
    void HandleChestMove(ClientState& client, const glm::ivec3& pos, ItemId item, uint32_t count, bool intoChest);
    // Their open chest, if it's still there and in reach (nullptr: none).
    Inventory* OpenChestOf(ClientState& client);
    void SendChest(ClientState& client);         // what's in their open chest (or that it's closed)
    ChestItems& ChestAt(const glm::ivec3& pos);  // its contents (a dungeon chest's loot is rolled the first time)
    void SendChestToViewers(const glm::ivec3& pos);
    void RemoveChest(const glm::ivec3& pos, ClientState* breaker); // gone: its contents spill out where it was if someone broke it
    void DropOnDeath(ClientState& client); // half of what they carry, into a chest where they died
    // Right-clicked a block that does something: a bed (respawn, sleep) or a door.
    void HandleUseBed(ClientState& client, const glm::ivec3& pos);
    void ToggleDoor(const glm::ivec3& pos, bool open); // both halves (not shut on anyone)
    void MirrorDoor(const glm::ivec3& pos);             // its hinge to the other side (both halves)
    // Fall damage, from each accepted position: landing far below the highest point
    // since last on the ground hurts. `reset`: start counting from here (teleports,
    // the first position, flying).
    void UpdateFall(ClientState& client, const net::PlayerState& state, bool reset);
    // Where they come back after dying: on (or beside) their bed if it's still there and
    // there's room, otherwise the world's spawn point (`why` says what was wrong).
    glm::vec3 RespawnPoint(const ClientState& client, std::string& why) const;
    // Damage in half hearts. `armourHelps`: a hit or a trap (armour takes some off),
    // not drowning, bad air or starving.
    void Hurt(ClientState& client, int amount, const std::string& deathMessage, bool armourHelps = true);
    void SendHealth(ClientState& client, bool hurt);
    // Advances the clock and the weather by one tick.
    void UpdateSky();
    // How far a player can break and build: 6 blocks in survival, 50 in creative (plus slack).
    float EditReach(const ClientState& client) const;
    bool InReach(const ClientState& client, const glm::ivec3& pos, float reach) const;
    // Re-sends the authoritative block to the client (after a refused edit).
    void Echo(ClientState& client, const glm::ivec3& pos);
    void Kick(ClientState& client, const std::string& reason);

    void Tick();
    void DistributeChanges();
    void StreamWorld(ClientState& client);
    void BroadcastPlayers();
    // Sends a whole chunk; returns its encoded size.
    size_t SendChunk(ClientState& client, const glm::ivec3& coords);
    void Log(const std::string& message) const;
    // Saves edited chunks and connected players' positions, and writes them out.
    void Save(bool shuttingDown);
    void StorePlayer(const ClientState& client);

    Options options;
    net::NetServer net;
    TerrainGenerator terrain;
    CreatureSystem creatures;
    std::unordered_map<glm::ivec3, ChestItems, Coords::IVec3Hash> chests;
    // Planted crops still growing (saved with the world), and when they're next looked at.
    std::unordered_set<glm::ivec3, Coords::IVec3Hash> crops;
    double nextCropUpdate = 0.0;
    void UpdateCrops();
    bool GrowTree(const glm::ivec3& at, bool pine); // a sapling grows (false: no room yet)
    // Gives them an achievement if they haven't got it (survival only): a toast for them,
    // a line for everyone else.
    void Award(ClientState& client, Achievement a);
    // Arrows: flying (with gravity) until they hit a block (then stuck there a while), a
    // creature or a player.
    struct Arrow {
        glm::vec3 position, velocity;
        uint16_t shooter;
        double born, stuckAt = -1.0; // stuckAt >= 0: stuck in a block since then
        int damage = 5;              // (a crossbow's bolt hits harder)
    };
    std::vector<Arrow> arrows;
    // Items lying about (saved with the world): picked up by players who touch them,
    // and what a block left flies to whoever broke it.
    DroppedItems dropped;
    // Lava (world/Lava.h), stepped every Lava::kInterval, and steam in the air
    // (server/Steam.h), every SteamCloud::kInterval and sent out a few times a second.
    SteamCloud steam;
    double nextLava = 0.0, nextSteam = 0.0, nextSteamSend = 0.0;
    void UpdateLava();
    // Fire (world/Fire.h): when each was lit (it dies down a while after it has nothing to
    // burn), the next step, and lighting one (false: it can't be there).
    std::unordered_map<glm::ivec3, double, Coords::IVec3Hash> fireLit;
    double nextFire = 0.0;
    uint32_t fireRoll = 0x9E3779B9u;
    float FireRandom() { fireRoll = fireRoll * 1664525u + 1013904223u; return (fireRoll >> 8) / float(1u << 24); }
    bool Ignite(const glm::ivec3& pos);
    // Sets fire, now and then, to open cells round `pos` beside something that burns.
    void Scorch(const glm::ivec3& pos, int radius, float chance);
    void UpdateFire();
    // Leaves no log holds up any more (world/Trees.h), and when each falls away.
    std::unordered_map<glm::ivec3, double, Coords::IVec3Hash> leafFalls;
    std::unordered_set<glm::ivec3, Coords::IVec3Hash> placedLeaves; // put down by players: they stay (saved)
    std::unordered_set<glm::ivec3, Coords::IVec3Hash> deathChests;  // left where players died: fire spares them (saved)
    void LoosenLeaves(const std::vector<glm::ivec3>& logsGone);
    void UpdateLeaves();
    void UpdateSteam();
    void SendSteam();
    void UpdateDroppedItems();
    void SendDroppedItems();
    void HandleShoot(ClientState& client, const glm::vec3& direction);
    void UpdateArrows();
    void SendArrows();
    void HandleCast(ClientState& client, const glm::ivec3& pos, bool cast);
    // Signs' texts, by position (saved with the world).
    std::unordered_map<glm::ivec3, std::string, Coords::IVec3Hash> signs;
    void HandleWriteSign(ClientState& client, const glm::ivec3& pos, const std::string& text);
    // TNT burning down: where, and when it goes.
    std::unordered_map<glm::ivec3, double, Coords::IVec3Hash> fuses;
    void LightTnt(const glm::ivec3& pos, double fuse);
    void UpdateTnt();
    void Explode(const glm::ivec3& pos, int radius);
    void BroadcastSign(const glm::ivec3& pos);
    // Signs go in many ways besides being broken (washed away, lasered, filled over,
    // knocked off): once a second, texts whose sign is gone are forgotten, and clients told.
    void ForgetLostSigns();
    double nextSignSweep = 0.0;
    void UpdateFishing(); // bites, and lines snapped by walking off

    // Air power (world/Power.h; the code is in Power.cpp).
    struct Wheel {
        float flow = 0.0f;  // water units a second down one side more than the other (smoothed)
        float speed = 0.0f; // 0..1
        bool jammed = false;
        bool driving = false; // on a shaft line into a compressor
    };
    struct Drill {
        double nextDig = 0.0;
        double lastDug = -1e9;
        int depth = 0; // how deep the last block it dug was
        Power::DrillState state = Power::DrillState::Idle;
    };
    struct Smelter {
        double nextSmelt = 0.0;
        double lastSmelt = -1e9;
        Power::DrillState state = Power::DrillState::Idle; // Working, NoPressure, NoChest, or Idle (nothing to smelt)
        ItemId made = Items::kNone;                           // what it made last
    };
    struct Compressor {
        float power = 0.0f; // the speeds of the wheels on its shaft lines, added up
        int wheels = 0, turning = 0;
    };
    struct AirNetwork {
        int pipes = 0, tanks = 0, compressors = 0, drills = 0, working = 0, pumps = 0, pumping = 0;
        bool endless = false; // a creative air tank keeps it full
        float air = 0.0f, capacity = 0.0f, supply = 0.0f; // supply: air a second from its compressors
        float Pressure() const { return capacity > 0.0f ? air / capacity : 0.0f; }
    };
    std::unordered_map<glm::ivec3, Wheel, Coords::IVec3Hash> wheels;
    std::unordered_map<glm::ivec3, Drill, Coords::IVec3Hash> drills;
    std::unordered_map<glm::ivec3, Smelter, Coords::IVec3Hash> smelters;
    // Extractors, crafters and harvesters: when they next work, and how it went.
    struct Worker {
        double next = 0.0;
        double last = -1e9;
        Power::DrillState state = Power::DrillState::Idle; // Blocked: a crafter with no recipe
        ItemId cursor = 0;                                 // an extractor's place in its chest (the last item it moved)
    };
    std::unordered_map<glm::ivec3, Worker, Coords::IVec3Hash> workers;
    // Fluids (world/Fluids.h): what each pipe and tank holds (saved), and the networks
    // as of the last update.
    std::unordered_map<glm::ivec3, int, Coords::IVec3Hash> fluidHeld;
    std::unordered_map<glm::ivec3, Fluids::Kind, Coords::IVec3Hash> fluidKind;
    // Pumps', drains' and fluid outlets' allowances (server/Water.h SetDrainBudget): a
    // part of the network each was granted from, so what it trades goes to that network.
    std::unordered_map<glm::ivec3, glm::ivec3, Coords::IVec3Hash> fluidGrants;
    struct FluidNet {
        Fluids::Kind kind = Fluids::Kind::None;
        int amount = 0, capacity = 0;
        std::vector<glm::ivec3> members;
        bool lavaProof = false; // all obsidian pipes: may hold lava
        int Room() const { return capacity - amount; }
        bool Takes(Fluids::Kind k) const { return kind == Fluids::Kind::None || kind == k || amount == 0; }
    };
    std::vector<FluidNet> fluidNets;
    std::unordered_map<glm::ivec3, int, Coords::IVec3Hash> fluidNetOf;
    std::unordered_map<glm::ivec3, bool, Coords::IVec3Hash> engines; // running (had fuel) at the last update
    std::unordered_map<glm::ivec3, bool, Coords::IVec3Hash> diffusers; // air diffusers: running (had the air) at the last update
    // Whether a working air diffuser's fresh air reaches this point (eyes).
    bool FreshAirAt(const glm::vec3& eye) const;
    // Works out the fluid networks (from every tank and every machine's neighbours).
    void BuildFluidNets();
    // The network on that side of a position (nullptr: none).
    FluidNet* FluidNetAt(const glm::ivec3& pos);
    // The first network beside a machine that will take this kind with room for some,
    // or (have = true) that holds some of it.
    FluidNet* FluidNetBeside(const glm::ivec3& pos, Fluids::Kind kind, bool have);
    // Puts each network's fluid back into its parts (shared by size).
    void StoreFluids();
    bool PumpOil(const glm::ivec3& pos, Worker& w);
    // Multiblock structures (world/Multiblocks.h; the code is in Multiblocks.cpp).
    // Striking a block with the hammer: forms the structure it's part of, if it's
    // complete (else says what's missing). Taking one apart (any of it broken): every
    // block back as it was; what the block at `pos` was.
    void TryForm(ClientState* client, const glm::ivec3& pos); // (client: who's told; nobody: the log)
    Block TakeApart(const glm::ivec3& pos);
    // The core of the formed structure a block is part of (false: none).
    bool CoreOf(const glm::ivec3& pos, glm::ivec3& core) const;
    // A machine's state as of the last power update (a structure's part: its core's),
    // for machine sensors (none: not a machine).
    std::optional<Power::DrillState> MachineState(const glm::ivec3& pos) const;
    // Whether a formed structure is still whole (its parts all there).
    bool StructureWhole(const glm::ivec3& core, const Block& block) const;
    // A structure's turn (it has the air): pumping, refining or smelting.
    bool RunStructure(const glm::ivec3& core, const Block& block, Worker& w);
    static constexpr int kStructureWellDepth = 160;
    static constexpr int kStructureCrude = 48;     // a derrick structure's draw every half second
    static constexpr int kStructureRefine = 96;    // a refinery's every update
    // A refinery leaves a bitumen in its inventory for every this much crude it refines
    // (what it's refined toward the next, by its core: not saved).
    static constexpr int kCrudePerBitumen = 256;
    std::unordered_map<glm::ivec3, int, Coords::IVec3Hash> refinedTowardBitumen;
    static constexpr double kFurnaceInterval = 0.5;
    bool Refine(const glm::ivec3& pos, const Block& refinery, Worker& w);
    std::unordered_map<glm::ivec3, Compressor, Coords::IVec3Hash> compressors;
    std::unordered_map<glm::ivec3, Power::DrillState, Coords::IVec3Hash> pumpStates;
    std::unordered_map<glm::ivec3, Power::DrillState, Coords::IVec3Hash> pistonStates; // Idle (in), Working (out), NoPressure, Blocked
    // Air launchers: whether each one's channel was on last time (it fires as it comes on),
    // and when it last fired (for the clients: a thunk and a puff).
    std::unordered_map<glm::ivec3, bool, Coords::IVec3Hash> launcherArmed;
    std::unordered_map<glm::ivec3, double, Coords::IVec3Hash> launcherFired;
    void Launch(const glm::ivec3& pos, const Block& launcher, float speed);
    // Fire (or, with lava, lava too) within `range` of a point (a sphere; below: down to `down`).
    bool HeatNear(const glm::ivec3& pos, int range, int down, bool lava) const;
    // A sprinkler at pos sprays (douse): fires round and below it out, and players there.
    // Returns whether there was fire there (with douse false, only looks).
    bool Sprinkle(const glm::ivec3& pos, bool douse);
    std::unordered_map<glm::ivec3, float, Coords::IVec3Hash> air; // air held in each part of a network
    std::vector<AirNetwork> networks;                             // as of the last update
    std::unordered_map<glm::ivec3, int, Coords::IVec3Hash> networkOf; // each part's network, as of the last update
    std::vector<glm::ivec3> machineList;                          // (update scratch)
    std::unordered_map<glm::ivec3, glm::vec2, Coords::IVec3Hash> wheelFlow; // (update scratch)
    double nextPowerUpdate = 0.0;
    void UpdatePower();
    // Whether a wheel's paddle cells are clear.
    bool WheelClear(const glm::ivec3& pos, const Block& wheel) const;
    // A drill's turn to dig (it has the air): digs the next block under it into a chest
    // beside it. Returns whether it dug.
    bool DrillDig(const glm::ivec3& pos, Drill& drill);
    // The chests joined to a machine: touching it, or at the ends of item pipes from it
    // (nearest first). For things of `item` (filter pipes pass only theirs; none:
    // anything, through unset filters only). With `face`, only out of that side.
    std::vector<glm::ivec3> ChestsJoined(const glm::ivec3& pos, ItemId item = Items::kNone, const glm::ivec3* face = nullptr) const;
    // An extractor's, a crafter's or a harvester's turn (it has the air): whether it did
    // something.
    bool Extract(const glm::ivec3& pos, const Block& extractor, Worker& w);
    bool CraftAt(const glm::ivec3& pos, const Block& crafter, Worker& w);
    bool Harvest(const glm::ivec3& pos, Worker& w);
    bool Crush(const glm::ivec3& pos, Worker& w);
    // A smelter's turn (it has the air): smelts a block in a chest joined to it.
    // Returns whether it did.
    bool Smelt(const glm::ivec3& pos, Smelter& smelter);
    // A piston pushing out (the blocks in front of it, up to Power::kPushLimit, a block
    // further, and its head into the space): whether it could. And pulling back (the
    // block in front of its head with it).
    bool PistonPush(const glm::ivec3& pos, const Block& piston);
    void PistonPull(const glm::ivec3& pos, const Block& piston);
    // Whether a piston can move this block (things with a state of their own kept by
    // the server, machinery and liquids can't be).
    bool Movable(const glm::ivec3& pos, const Block& b) const;
    void SendMachines();

    // Signals (world/Signals.h; the code is in Signals.cpp).
    struct SignalBlock {
        Signals::Config config;
        bool on = false;       // a gate's output; a sluice gate's open
        double clockAt = 0.0;  // a clock's next flip
    };
    std::unordered_map<glm::ivec3, SignalBlock, Coords::IVec3Hash> signalBlocks;
    std::unordered_map<glm::ivec3, double, Coords::IVec3Hash> plateSignal; // plates sending until then
    double nextSignalUpdate = 0.0;
    void UpdateSignals();
    // Mining posts (world/Block.h; the code is in Miners.cpp): while a post holds pay
    // (cooked food), up to kMaxMiners dwarf miners come and work it, each digging the
    // nearest ore within kPostRadius of it every kMinerInterval seconds into the post,
    // for kMinerPay units of food. Unpaid for kUnpaidLeave seconds, they leave.
    static constexpr int kMaxMiners = 2;
    static constexpr int kPostRadius = 12;
    static constexpr double kMinerInterval = 5.0;
    static constexpr uint32_t kMinerPay = 16; // a quarter of a block of food for each ore
    static constexpr double kUnpaidLeave = 30.0;
    struct MiningPost {
        std::vector<uint16_t> miners;
        std::vector<double> nextDig;
        double nextArrival = 0.0, unpaidSince = -1.0;
        Power::DrillState state = Power::DrillState::Idle; // Working, NoPressure (no pay), Done (no ore left in reach)
    };
    std::unordered_map<glm::ivec3, MiningPost, Coords::IVec3Hash> miningPosts;
    double nextMinersUpdate = 0.0;
    void UpdateMiningPosts();
    // VOXEL_BENCH timings (Tick): seconds in each part this second, ticks, the worst.
    double benchSections[6] = {};
    int benchTicks = 0;
    double benchWorst = 0.0, benchLast = 0.0;
    // A pressure plate pressed (by a player or a creature): it sends for a while. A
    // dungeon flood chamber's gets its wiring the first time.
    void PressPlate(const glm::ivec3& pos, double now);
    std::unordered_map<uint16_t, double> creatureSpikeAt; // creatures' next spike hits
    // Horde nights (see CreatureSystem::SetHorde): one night in options.hordeOdds (never the first), announced
    // at dusk; operators can start one with /horde.
    bool wasNight = false;
    // Nights begun in this world (saved): the first has no horde and no raids.
    uint32_t nights = 0;
    void UpdateHorde();
    // Night falls: roll for a horde (or raids). Once a night, at dusk or when someone
    // gets into bed before that, so sleeping early can't dodge the roll.
    void BeginNight();
    void SetHorde(bool on);
    // Whether a sensor senses what it's for (world/Signals.h).
    bool Senses(const glm::ivec3& pos, const Signals::Config& c) const;
    void HandleConfigureSignal(ClientState& client, const net::SignalInfo& m);
    net::SignalInfo SignalInfoAt(const glm::ivec3& pos) const;
    void BroadcastSignal(const glm::ivec3& pos);
    // Shuts an open sluice gate, moving the water in its cell out of the way (false:
    // nowhere for it to go yet).
    bool CloseSluice(const glm::ivec3& pos);
    // Moves the water out of a cell into the cells above and beside it that have room
    // (none of it is lost): whether it all went (if not, nothing moves).
    bool PushWaterAside(const glm::ivec3& pos);
    void HandleInspect(ClientState& client, const glm::ivec3& pos);

    double lastCreatureTime = 0.0;
    std::unique_ptr<WorldStorage> storage; // declared before `world`, which uses it
    std::unique_ptr<ServerWorld> world;
    double lastSaveTime = 0.0;
    double lastTrapsTime = 0.0; // last UpdateTraps (breath runs on real time)

    double timeOfDay = 0.05;     // 0..1, see net::PlayerSnapshot
    bool clockPaused = false;    // /time pause: the day stands still (saved with the world)
    net::GameMode defaultMode = net::GameMode::Survival; // new players' (from the save, or the options)
    Options::Weather weatherMode = Options::Weather::Auto; // starts as options.weather; /weather changes it
    bool raining = false;
    bool storm = false;          // the rain is a thunderstorm
    double nextLightning = 0.0;
    void UpdateStorm();          // lightning strikes near players while a storm rages
    float rainLevel = 0.0f;      // 0..1, fades toward `raining`
    // Springs follow the weather: faster in rain, faster still in storms, and slow to
    // a trickle in a drought (kDroughtDays without rain). How long it's been dry.
    double dryFor = 0.0;
    bool drought = false;
    static constexpr double kDroughtDays = 2.0;
    static constexpr float kRainFlow = 1.6f, kStormFlow = 2.5f, kDroughtFlow = 0.35f;
    float SpringFlow() const { return raining ? (storm ? kStormFlow : kRainFlow) : drought ? kDroughtFlow : 1.0f; }
    double weatherTimer = 0.0;   // seconds until the weather changes (Auto)
    uint32_t weatherSeed = 0x9E3779B9u;
    std::unordered_map<net::PeerId, ClientState> clients;
    uint16_t nextPlayerId = 1;
    uint32_t tickCount = 0;
    double startTime = 0.0;

    std::vector<glm::ivec3> changedBlocks, changedLight; // tick scratch
    std::unique_ptr<net::ChunkBlocks> blockScratch;
    std::unique_ptr<net::ChunkLight> lightScratch;
    std::vector<glm::ivec3> loadOffsets;                 // nearest first

    std::unordered_set<std::string> operators; // lower case
    std::unordered_map<glm::ivec3, double, Coords::IVec3Hash> crumbling;   // crumbling bricks about to fall, and when
    std::mutex consoleMutex;
    std::vector<std::string> consoleQueue;

    std::atomic<bool> running{false};
    std::thread thread;
};
