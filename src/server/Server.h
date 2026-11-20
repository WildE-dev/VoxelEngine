#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "net/Protocol.h"
#include "net/Transport.h"
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
class Server
{
public:
    static constexpr int kTickRate = 20;
    static constexpr size_t kMaxClients = 16;

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
        bool allSent = false; // every chunk in range has been sent (until the centre moves)

        bool hasState = false; // has sent a valid PlayerState since joining

        // Chunks the client has, with the version of the light it has for each.
        std::unordered_map<glm::ivec3, uint32_t, Coords::IVec3Hash> sent;
        // Changes not sent yet, for chunks the client has: cells (chunk-local indices)
        // and chunks whose light changed.
        std::unordered_map<glm::ivec3, std::unordered_set<uint16_t>, Coords::IVec3Hash> pendingCells;
        std::unordered_set<glm::ivec3, Coords::IVec3Hash> pendingLight;
    };

    void HandleEvent(const net::NetEvent& event);
    void HandleMessage(ClientState& client, const std::vector<uint8_t>& data);
    void HandleHello(ClientState& client, const net::Hello& hello);
    void HandlePlayerState(ClientState& client, const net::PlayerState& state);
    void HandleBreak(ClientState& client, const glm::ivec3& pos);
    void HandlePlace(ClientState& client, const glm::ivec3& pos, BlockType type);
    void HandleCarve(ClientState& client, const net::EditCarve& carve);
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
    std::unique_ptr<WorldStorage> storage; // declared before `world`, which uses it
    std::unique_ptr<ServerWorld> world;
    double lastSaveTime = 0.0;
    std::unordered_map<net::PeerId, ClientState> clients;
    uint16_t nextPlayerId = 1;
    uint32_t tickCount = 0;
    double startTime = 0.0;

    std::vector<glm::ivec3> changedBlocks, changedLight; // tick scratch
    std::unique_ptr<net::ChunkBlocks> blockScratch;
    std::unique_ptr<net::ChunkLight> lightScratch;
    std::vector<glm::ivec3> loadOffsets;                 // nearest first

    std::atomic<bool> running{false};
    std::thread thread;
};
