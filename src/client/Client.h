#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "net/Protocol.h"
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
    };

    explicit Client(ClientWorld& world);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Connects and sends Hello (blocks up to a few seconds). The server's Welcome
    // arrives through Poll.
    bool Connect(const std::string& host, uint16_t port, const std::string& name, std::string& error);
    void Disconnect();

    // Handles everything that has arrived. Call once per frame.
    void Poll();

    bool IsConnected() const { return net.IsConnected(); }
    bool IsWelcomed() const { return welcomed; }
    // Why the connection ended (empty while connected).
    const std::string& GetDisconnectReason() const { return disconnectReason; }
    uint16_t GetPlayerId() const { return playerId; }
    glm::vec3 GetSpawn() const { return spawn; }

    // A position correction from the server, once.
    std::optional<glm::vec3> TakeCorrection();

    // Sent at most kStateRate times a second; extra calls are ignored.
    void SendPlayerState(const glm::vec3& feet, const glm::vec3& velocity, float yaw, float pitch, uint8_t flags);

    // Edits (predicted locally).
    void BreakBlock(const glm::ivec3& pos);
    void PlaceBlock(const glm::ivec3& pos, BlockType type);
    void Carve(const std::vector<glm::ivec3>& centers, int radius);

    // Other players, interpolated kInterpolationDelay seconds in the past so movement is
    // smooth between snapshots.
    std::vector<RemotePlayer> GetRemotePlayers() const;

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
    static double Now();

    ClientWorld& world;
    net::NetClient net;
    bool welcomed = false;
    std::string disconnectReason;
    uint16_t playerId = 0;
    glm::vec3 spawn{0.0f};

    uint32_t stateSequence = 0;
    uint16_t correctionId = 0;
    std::optional<glm::vec3> pendingCorrection;
    double lastStateSent = 0.0;

    struct Sample {
        double time;
        RemotePlayer state;
    };
    std::unordered_map<uint16_t, std::deque<Sample>> remote;
    uint32_t lastSnapshotTick = 0;

    std::unique_ptr<net::ChunkBlocks> blocks;
    std::unique_ptr<net::ChunkLight> light;
    Stats stats;
};
