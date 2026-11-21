#pragma once

#include <array>
#include <atomic>
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
#include "world/Breathing.h"
#include "world/Hunger.h"
#include "world/Crafting.h"
#include "world/Coords.h"
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
// survival, every block costs resources (units of its type, see Block::ResourceUnits)
// that players collect by breaking blocks, and traps hurt. The server keeps everyone's
// mode and resources (saved with the player) and refuses what they can't afford.
//
// Crafting (world/Crafting.h): survival players turn resources into other blocks and
// make tools, which they need to have to use.
//
// Creatures (server/Creatures.h): deer and ghouls, run here and sent to the players
// near them every tick; players hit them with Attack.
//
// Mining (HandleMineStart, HandleBreak): in survival breaking a block takes time
// (world/Mining.h). The client says when it starts on a block, and the break only
// goes through once that long has passed.
//
// Health and traps (UpdateTraps): players have kMaxHealth half hearts. Standing in
// spikes hurts; crumbling bricks give way soon after someone stands on them (and the
// collapse spreads to crumbling bricks touching them); a pressure plate opens every
// sluice gate within kSluiceReach. At no health a player respawns at the spawn point.
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

    static constexpr int kMaxHealth = 20;           // half hearts
    static constexpr int kSpikeDamage = 4;          // two hearts...
    static constexpr double kSpikeInterval = 0.5;   // ...every half second
    static constexpr double kCrumbleDelay = 0.4;    // after first being stood on
    static constexpr double kCrumbleSpread = 0.12;  // to each crumbling brick touching it
    static constexpr int kSluiceReach = 8;          // blocks (per axis) a plate reaches
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
        enum class Weather { Auto, Clear, Rain } weather = Weather::Auto;
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
        // Survival players who die leave half of what they carry in a chest there.
        bool dropOnDeath = true;
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
        std::array<uint32_t, static_cast<size_t>(BlockType::COUNT)> resources{}; // units held (survival)
        uint8_t tools = 0; // tools made (Crafting::ToolBit each; creative has them all)
        uint8_t armour = 0; // armour worn (Armour::Bit each)
        std::optional<glm::ivec3> openChest; // the chest they have open
        std::optional<glm::ivec3> bed;       // where they respawn (if it's still a bed)
        double sleptAt = -1000.0;            // when they last got into bed at night
        float fallPeak = 0.0f;               // highest point since last on the ground (UpdateFall)
        bool falling = false;
        Tool heldTool = Tool::None;          // in their hand, for others to see
        BlockType heldBlock = BlockType::AIR;

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
    void HandleCraft(ClientState& client, int recipe, int count);
    // The tool a player really works with: theirs if they have it, a bare hand if not.
    static Tool Usable(const ClientState& client, Tool tool);
    // A tick of the creatures, and telling each player about the ones near them.
    void UpdateCreatures();
    void SendCreatures();
    void HandlePlace(ClientState& client, const glm::ivec3& pos, BlockType type, int height, int faceAxis);
    void HandleChisel(ClientState& client, const net::EditChisel& chisel);
    void HandleCarve(ClientState& client, const net::EditCarve& carve);
    void HandleSkipTime(ClientState& client);
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
    // Adds or removes a name; returns false if nothing changed. Saves ops.txt.
    bool SetOperator(const std::string& name, bool op);
    void LoadOperators();
    void SaveOperators() const;
    // Moves a player (they're told with a correction, like a rejected move).
    void Teleport(ClientState& client, const glm::vec3& feet);
    void SetTimeOfDay(double t) { timeOfDay = t - std::floor(t); }
    // Tells a client what it may do (after joining, /op, /deop, /noclip).
    void SendAbilities(ClientState& client);
    // Survival bookkeeping: tells the client its mode / what it holds.
    void SendMode(ClientState& client);
    void SendResources(ClientState& client);
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
    void HandleEat(ClientState& client, BlockType food);
    // Chests: what's in each (units by type), by position; saved with the world.
    using ChestItems = std::array<uint32_t, static_cast<size_t>(BlockType::COUNT)>;
    void HandleOpenChest(ClientState& client, const glm::ivec3& pos, bool open);
    void HandleChestMove(ClientState& client, const glm::ivec3& pos, BlockType type, uint32_t units, bool intoChest);
    void SendChest(ClientState& client);         // what's in their open chest (or that it's closed)
    ChestItems& ChestAt(const glm::ivec3& pos);  // its contents (a dungeon chest's loot is rolled the first time)
    void SendChestToViewers(const glm::ivec3& pos);
    void RemoveChest(const glm::ivec3& pos, ClientState* breaker); // gone: contents to whoever broke it
    void DropOnDeath(ClientState& client); // half of what they carry, into a chest where they died
    void HandleUseBed(ClientState& client, const glm::ivec3& pos);
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
    // A pressure plate at `plate` was stepped on: opens the sluice gates near it.
    void TriggerPlate(ClientState& client, const glm::ivec3& plate);
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
    double lastCreatureTime = 0.0;
    std::unique_ptr<WorldStorage> storage; // declared before `world`, which uses it
    std::unique_ptr<ServerWorld> world;
    double lastSaveTime = 0.0;
    double lastTrapsTime = 0.0; // last UpdateTraps (breath runs on real time)

    double timeOfDay = 0.05;     // 0..1, see net::PlayerSnapshot
    net::GameMode defaultMode = net::GameMode::Survival; // new players' (from the save, or the options)
    Options::Weather weatherMode = Options::Weather::Auto; // starts as options.weather; /weather changes it
    bool raining = false;
    float rainLevel = 0.0f;      // 0..1, fades toward `raining`
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
    std::unordered_map<glm::ivec3, double, Coords::IVec3Hash> plateReady;  // plates on cooldown until then
    std::mutex consoleMutex;
    std::vector<std::string> consoleQueue;

    std::atomic<bool> running{false};
    std::thread thread;
};
