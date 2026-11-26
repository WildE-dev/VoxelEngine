#pragma once
// The relay (VoxelRelay): lets friends join a game behind a home router, with a join
// code, by passing UDP datagrams between them and the host (see RelayProtocol.h).
// No dependencies beyond the system's sockets, so on a Linux VPS it builds alone:
//   g++ -O2 -std=c++17 -o voxel-relay src/relay/main.cpp src/relay/RelayServer.cpp
#include <cstdint>
#include <memory>
#include <string>

namespace relay {

struct RelayConfig {
    std::string bindHost = "0.0.0.0";
    uint16_t port = 25700;          // 0: any free port (tests)
    int maxRooms = 200;
    int maxRoomsPerAddress = 4;     // games one address can have open at once
    int maxJoinersPerRoom = 32;
    double hostTimeoutSeconds = 30.0;   // a host silent this long has gone
    double joinerTimeoutSeconds = 60.0; // likewise a joiner (ENet pings every second)
    int joinAttemptsPerMinute = 20;     // per address, so codes can't be guessed quickly
    bool log = true;
};

class RelayServer {
public:
    RelayServer();
    ~RelayServer();
    RelayServer(const RelayServer&) = delete;
    RelayServer& operator=(const RelayServer&) = delete;

    bool Start(const RelayConfig& config, std::string& error);
    uint16_t GetPort() const;
    // Handles what arrives within timeoutMs (and forgets the silent).
    void RunOnce(int timeoutMs);
    int RoomCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace relay
