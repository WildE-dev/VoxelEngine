#pragma once
// Playing on the local network: a game opened to LAN, and finding the games that are.
//
// LanHost makes a game reachable from other machines on the network and announces it:
// every second it broadcasts a short datagram (see Lan.cpp) to UDP port
// kLanDiscoveryPort on each network the machine is on. With `forward` (singleplayer,
// whose server only listens on 127.0.0.1) it also listens on every interface, on the
// game's default port if it's free, and passes each player's datagrams to the server
// over a loopback socket of its own, so to the server they're just more local clients.
// A dedicated server already listens everywhere: it only needs announcing.
//
// LanBrowser listens on kLanDiscoveryPort and keeps the games heard from lately. Both
// run on threads of their own.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace net {

constexpr uint16_t kLanDiscoveryPort = 27451;

struct LanGame {
    std::string name;      // who's hosting ("MossyRaven83")
    std::string world;     // the world's name
    std::string address;   // where it was heard from (IPv4)
    uint16_t port = 0;     // to connect to there
    int players = -1;      // -1: not said (a dedicated server)
    bool compatible = true; // the same protocol version as this game
};

class LanHost {
public:
    LanHost() = default;
    ~LanHost();
    LanHost(const LanHost&) = delete;
    LanHost& operator=(const LanHost&) = delete;

    // Only this machine (tests: no firewall asking about them), not the network.
    bool loopbackOnly = false;

    // serverPort: the game's server. forward: listen on every interface and pass players
    // on to it (its port is GetPort); otherwise just announce serverPort.
    bool Start(const std::string& name, const std::string& world, uint16_t serverPort, bool forward, std::string& error,
               uint16_t discoveryPort = kLanDiscoveryPort);
    void Stop(); // (says goodbye, so browsers drop it at once)
    bool Running() const { return thread.joinable(); }
    uint16_t GetPort() const { return port; }
    int PlayerCount() const { return players.load(); } // forwarded players
    // This machine's addresses on its networks, for players to type in by hand.
    std::vector<std::string> GetAddresses() const;

private:
    void Run();

    std::thread thread;
    std::atomic<bool> stopping{ false };
    std::atomic<int> players{ 0 };
    std::string name, world;
    uint16_t serverPort = 0, port = 0, discoveryPort = kLanDiscoveryPort;
    bool forward = false;
    uint64_t listenSocket = ~0ull; // (the platform's socket, kept out of this header)
};

class LanBrowser {
public:
    LanBrowser() = default;
    ~LanBrowser();
    LanBrowser(const LanBrowser&) = delete;
    LanBrowser& operator=(const LanBrowser&) = delete;

    bool loopbackOnly = false; // (as LanHost's)

    bool Start(std::string& error, uint16_t discoveryPort = kLanDiscoveryPort);
    void Stop();
    std::vector<LanGame> GetGames() const; // heard from in the last few seconds

private:
    void Run();

    struct Heard {
        uint64_t id = 0;
        LanGame game;
        double last = 0.0;
    };
    std::thread thread;
    std::atomic<bool> stopping{ false };
    mutable std::mutex mutex;
    std::vector<Heard> heard;
    uint64_t socket = ~0ull;
};

// This machine's IPv4 addresses on its networks (not loopback).
std::vector<std::string> LocalAddresses();

} // namespace net
