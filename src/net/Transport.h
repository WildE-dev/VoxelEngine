#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A thin wrapper over ENet (reliable UDP). Keeps ENet out of every other header.
// Each NetServer / NetClient must be used from one thread at a time.
namespace net {

using PeerId = uint32_t;

struct NetEvent {
    enum class Type { Connect, Disconnect, Receive };
    Type type = Type::Receive;
    PeerId peer = 0;
    uint8_t channel = 0;
    std::vector<uint8_t> data; // Receive only
};

struct PeerStats {
    uint32_t roundTripMs = 0;
    size_t queuedReliableBytes = 0; // waiting to be sent or acknowledged
};

class NetServer {
public:
    NetServer();
    ~NetServer();
    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    // bindHost "" or "0.0.0.0" listens on every interface. Port 0 picks a free port
    // (see GetPort).
    bool Listen(const std::string& bindHost, uint16_t port, size_t maxClients);
    uint16_t GetPort() const;

    // Waits up to timeoutMs for an event (0 = just poll). Also sends queued packets.
    bool Poll(NetEvent& event, uint32_t timeoutMs);
    void Send(PeerId peer, uint8_t channel, const std::vector<uint8_t>& data);
    void Flush();
    // Graceful: the peer gets a Disconnect event once queued packets are delivered.
    void Disconnect(PeerId peer);
    PeerStats GetStats(PeerId peer) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class NetClient {
public:
    NetClient();
    ~NetClient();
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    // Blocks until connected or timeoutMs passes.
    bool Connect(const std::string& host, uint16_t port, uint32_t timeoutMs, std::string& error);
    bool IsConnected() const;
    bool Poll(NetEvent& event, uint32_t timeoutMs);
    void Send(uint8_t channel, const std::vector<uint8_t>& data);
    void Flush();
    // Sends a disconnect and waits briefly for it to be acknowledged.
    void Disconnect();
    PeerStats GetStats() const;
    uint64_t GetBytesReceived() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace net
