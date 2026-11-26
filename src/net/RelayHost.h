#pragma once
// Opens a game to friends through a relay (VoxelRelay, see relay/RelayProtocol.h): gets
// a join code, and passes each friend's datagrams between the relay and the game's
// server on this machine, each friend over a loopback socket of its own, so to the
// server a friend is just another client on 127.0.0.1. Runs on a thread of its own.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace net {

class RelayHost {
public:
    RelayHost() = default;
    ~RelayHost();
    RelayHost(const RelayHost&) = delete;
    RelayHost& operator=(const RelayHost&) = delete;

    // relay "host" or "host:port"; localPort the server's (listening on 127.0.0.1 or
    // every interface). False (and why) if the relay's address doesn't resolve.
    bool Start(const std::string& relay, uint16_t localPort, std::string& error);
    void Stop(); // (closes the room)
    bool Running() const { return thread.joinable(); }

    enum class State { Opening, Open, Refused };
    State GetState() const;
    std::string GetCode() const;   // once Open
    std::string GetStatus() const; // a line for the player: waiting, refused why, friends
    int FriendCount() const { return friends.load(); }

private:
    void Run();

    std::thread thread;
    std::atomic<bool> stopping{ false };
    std::atomic<int> friends{ 0 };
    mutable std::mutex mutex;
    State state = State::Opening;
    std::string code, refusal, relayName;
    uint32_t relayHost = 0;  // (ENet's form: network order)
    uint16_t relayPort = 0;
    uint16_t localPort = 0;
};

// "host" or "host:port" (the relay's port by default): false if there's no host.
bool SplitAddress(const std::string& text, uint16_t defaultPort, std::string& host, uint16_t& port);

} // namespace net
