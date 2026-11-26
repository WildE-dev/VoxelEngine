#include "net/Transport.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include <enet/enet.h>

#include "net/Protocol.h"
#include "relay/RelayProtocol.h"

namespace net {

namespace {

void EnsureEnetInitialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (enet_initialize() == 0) std::atexit(enet_deinitialize);
    });
}

// Ordered and reliable on the reliable channel; unreliable but sequenced (stale
// packets dropped) on the other.
ENetPacket* MakePacket(uint8_t channel, const std::vector<uint8_t>& data) {
    const enet_uint32 flags = channel == kChannelReliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    return enet_packet_create(data.data(), data.size(), flags);
}

// Queues a packet for a peer. ENet only takes the packet over if it queues it: one it
// refuses (the peer is on its way out, say: kicked, or timed out but not yet reported)
// is still ours to free.
void SendTo(ENetPeer* peer, uint8_t channel, const std::vector<uint8_t>& data) {
    ENetPacket* packet = MakePacket(channel, data);
    if (packet && enet_peer_send(peer, channel, packet) < 0) enet_packet_destroy(packet);
}

size_t QueuedReliableBytes(const ENetPeer* peer) {
    size_t bytes = peer->reliableDataInTransit;
    for (const ENetList* list : { &peer->outgoingCommands, &peer->outgoingSendReliableCommands }) {
        for (ENetListIterator it = enet_list_begin(list); it != enet_list_end(list); it = enet_list_next(it)) {
            bytes += reinterpret_cast<const ENetOutgoingCommand*>(it)->fragmentLength;
        }
    }
    return bytes;
}

// Fills `event` from an ENet event. Returns false for events with nothing to report.
// A busy machine (a big save, the whole test suite at once) can stall a process for
// seconds. ENet's defaults would drop a connection after 5 s without an answer and
// lose bursts of chunk data in small socket buffers; give both more room.
constexpr enet_uint32 kTimeoutMinimumMs = 20000, kTimeoutMaximumMs = 60000;
constexpr int kSocketBufferBytes = 4 * 1024 * 1024;

void Tolerate(ENetPeer* peer) {
    enet_peer_timeout(peer, 0, kTimeoutMinimumMs, kTimeoutMaximumMs);
}

void EnlargeBuffers(ENetHost* host) {
    if (!host) return;
    enet_socket_set_option(host->socket, ENET_SOCKOPT_RCVBUF, kSocketBufferBytes);
    enet_socket_set_option(host->socket, ENET_SOCKOPT_SNDBUF, kSocketBufferBytes);
}

template <typename PeerIdOf>
bool Translate(const ENetEvent& e, NetEvent& event, PeerIdOf peerIdOf) {
    switch (e.type) {
    case ENET_EVENT_TYPE_CONNECT:
        event.type = NetEvent::Type::Connect;
        event.peer = peerIdOf(e.peer);
        event.data.clear();
        return true;
    case ENET_EVENT_TYPE_DISCONNECT:
        event.type = NetEvent::Type::Disconnect;
        event.peer = peerIdOf(e.peer);
        event.data.clear();
        return true;
    case ENET_EVENT_TYPE_RECEIVE:
        event.type = NetEvent::Type::Receive;
        event.peer = peerIdOf(e.peer);
        event.channel = e.channelID;
        event.data.assign(e.packet->data, e.packet->data + e.packet->dataLength);
        enet_packet_destroy(e.packet);
        return true;
    default:
        return false;
    }
}

} // namespace

// --- Server -----------------------------------------------------------------------

struct NetServer::Impl {
    ENetHost* host = nullptr;
    PeerId nextId = 1;
    std::unordered_map<PeerId, ENetPeer*> peers;

    PeerId IdOf(ENetPeer* peer) {
        if (!peer->data) {
            const PeerId id = nextId++;
            peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
            peers[id] = peer;
        }
        return static_cast<PeerId>(reinterpret_cast<uintptr_t>(peer->data));
    }
    ENetPeer* Find(PeerId id) const {
        auto it = peers.find(id);
        return it != peers.end() ? it->second : nullptr;
    }
};

NetServer::NetServer() : impl(std::make_unique<Impl>()) {}

NetServer::~NetServer() {
    if (impl->host) {
        for (auto& [id, peer] : impl->peers) enet_peer_disconnect_now(peer, 0);
        enet_host_destroy(impl->host);
    }
}

bool NetServer::Listen(const std::string& bindHost, uint16_t port, size_t maxClients) {
    EnsureEnetInitialized();
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;
    if (!bindHost.empty() && bindHost != "0.0.0.0" && enet_address_set_host(&address, bindHost.c_str()) != 0) return false;
    impl->host = enet_host_create(&address, maxClients, kChannelCount, 0, 0);
    EnlargeBuffers(impl->host);
    return impl->host != nullptr;
}

uint16_t NetServer::GetPort() const {
    return impl->host ? impl->host->address.port : 0;
}

bool NetServer::Poll(NetEvent& event, uint32_t timeoutMs) {
    if (!impl->host) return false;
    ENetEvent e;
    while (enet_host_service(impl->host, &e, timeoutMs) > 0) {
        const bool disconnect = e.type == ENET_EVENT_TYPE_DISCONNECT;
        if (e.type == ENET_EVENT_TYPE_CONNECT) Tolerate(e.peer);
        const bool translated = Translate(e, event, [&](ENetPeer* p) { return impl->IdOf(p); });
        if (disconnect) {
            impl->peers.erase(event.peer);
            e.peer->data = nullptr;
        }
        if (translated) return true;
        timeoutMs = 0;
    }
    return false;
}

void NetServer::Send(PeerId id, uint8_t channel, const std::vector<uint8_t>& data) {
    if (ENetPeer* peer = impl->Find(id)) SendTo(peer, channel, data);
}

void NetServer::Flush() {
    if (impl->host) enet_host_flush(impl->host);
}

void NetServer::Disconnect(PeerId id) {
    if (ENetPeer* peer = impl->Find(id)) enet_peer_disconnect_later(peer, 0);
}

PeerStats NetServer::GetStats(PeerId id) const {
    PeerStats stats;
    if (const ENetPeer* peer = impl->Find(id)) {
        stats.roundTripMs = peer->roundTripTime;
        stats.queuedReliableBytes = QueuedReliableBytes(peer);
    }
    return stats;
}

// --- Client -----------------------------------------------------------------------

namespace {

// Asks a relay for the game opened with a join code, from the socket ENet will play on
// (the relay knows us by its address): until it says yes, no, or the time's up.
bool JoinThroughRelay(ENetSocket socket, const ENetAddress& relayAddress, const std::string& code, uint32_t timeoutMs, std::string& error) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    const auto join = relay::Writer(relay::Msg::Join).Str(relay::NormalizeCode(code)).bytes;
    auto lastAsked = Clock::now() - std::chrono::seconds(10);
    uint8_t reply[relay::kMaxDatagram];
    while (Clock::now() < deadline) {
        if (Clock::now() - lastAsked > std::chrono::milliseconds(500)) {
            lastAsked = Clock::now();
            ENetBuffer out;
            out.data = const_cast<uint8_t*>(join.data());
            out.dataLength = join.size();
            enet_socket_send(socket, &relayAddress, &out, 1);
        }
        enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
        if (enet_socket_wait(socket, &condition, 100) != 0 || !(condition & ENET_SOCKET_WAIT_RECEIVE)) continue;
        ENetAddress from;
        ENetBuffer in;
        in.data = reply;
        in.dataLength = sizeof reply;
        const int n = enet_socket_receive(socket, &from, &in, 1);
        if (n <= 0 || from.host != relayAddress.host || from.port != relayAddress.port || !relay::IsControl(reply, static_cast<size_t>(n))) continue;
        if (relay::TypeOf(reply) == relay::Msg::JoinOk) return true;
        if (relay::TypeOf(reply) == relay::Msg::Refused) {
            error.assign(reinterpret_cast<const char*>(reply + relay::kHeader), static_cast<size_t>(n) - relay::kHeader);
            return false;
        }
    }
    error = "no answer from the relay";
    return false;
}

} // namespace

struct NetClient::Impl {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    bool connected = false;
};

NetClient::NetClient() : impl(std::make_unique<Impl>()) {}

NetClient::~NetClient() {
    Disconnect();
    if (impl->host) enet_host_destroy(impl->host);
}

bool NetClient::Connect(const std::string& host, uint16_t port, uint32_t timeoutMs, std::string& error, const std::string& joinCode) {
    EnsureEnetInitialized();
    if (!impl->host) {
        impl->host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
        EnlargeBuffers(impl->host);
    }
    if (!impl->host) {
        error = "could not create a network socket";
        return false;
    }
    ENetAddress address;
    if (enet_address_set_host(&address, host.c_str()) != 0) {
        error = "unknown host '" + host + "'";
        return false;
    }
    address.port = port;
    if (!joinCode.empty() && !JoinThroughRelay(impl->host->socket, address, joinCode, timeoutMs, error)) return false;
    impl->peer = enet_host_connect(impl->host, &address, kChannelCount, 0);
    if (!impl->peer) {
        error = "no free peer slot";
        return false;
    }
    Tolerate(impl->peer);

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    ENetEvent e;
    while (Clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        const int result = enet_host_service(impl->host, &e, static_cast<enet_uint32>(std::max<long long>(1, remaining)));
        if (result < 0) break; // (the socket failed: waiting won't help)
        if (result == 0) continue;
        if (e.type == ENET_EVENT_TYPE_CONNECT) {
            impl->connected = true;
            return true;
        }
        if (e.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(e.packet);
        if (e.type == ENET_EVENT_TYPE_DISCONNECT) break;
    }
    enet_peer_reset(impl->peer);
    impl->peer = nullptr;
    error = "no answer from " + host + ":" + std::to_string(port);
    return false;
}

bool NetClient::IsConnected() const {
    return impl->connected;
}

bool NetClient::Poll(NetEvent& event, uint32_t timeoutMs) {
    if (!impl->host) return false;
    ENetEvent e;
    while (enet_host_service(impl->host, &e, timeoutMs) > 0) {
        if (e.type == ENET_EVENT_TYPE_DISCONNECT) {
            impl->connected = false;
            impl->peer = nullptr;
        }
        if (Translate(e, event, [](ENetPeer*) { return PeerId(0); })) return true;
        timeoutMs = 0;
    }
    return false;
}

void NetClient::Send(uint8_t channel, const std::vector<uint8_t>& data) {
    if (impl->connected && impl->peer) SendTo(impl->peer, channel, data);
}

void NetClient::Flush() {
    if (impl->host) enet_host_flush(impl->host);
}

void NetClient::Disconnect() {
    if (!impl->connected || !impl->peer) return;
    enet_peer_disconnect(impl->peer, 0);
    ENetEvent e;
    // Give the server a moment to acknowledge, so it drops us at once.
    bool acknowledged = false;
    for (int i = 0; i < 20 && !acknowledged; i++) {
        const int result = enet_host_service(impl->host, &e, 10);
        if (result < 0) break;
        if (result == 0) continue;
        if (e.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(e.packet);
        if (e.type == ENET_EVENT_TYPE_DISCONNECT) acknowledged = true;
    }
    if (!acknowledged) enet_peer_reset(impl->peer);
    impl->connected = false;
    impl->peer = nullptr;
}

PeerStats NetClient::GetStats() const {
    PeerStats stats;
    if (impl->peer) {
        stats.roundTripMs = impl->peer->roundTripTime;
        stats.queuedReliableBytes = QueuedReliableBytes(impl->peer);
    }
    return stats;
}

uint64_t NetClient::GetBytesReceived() const {
    return impl->host ? impl->host->totalReceivedData : 0;
}

} // namespace net
