#include "net/Transport.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include <enet/enet.h>

#include "net/Protocol.h"

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

size_t QueuedReliableBytes(const ENetPeer* peer) {
    size_t bytes = peer->reliableDataInTransit;
    for (ENetListIterator it = enet_list_begin(&peer->outgoingCommands); it != enet_list_end(&peer->outgoingCommands);
         it = enet_list_next(it)) {
        const auto* command = reinterpret_cast<const ENetOutgoingCommand*>(it);
        bytes += command->fragmentLength;
    }
    return bytes;
}

// Fills `event` from an ENet event. Returns false for events with nothing to report.
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
    if (ENetPeer* peer = impl->Find(id)) enet_peer_send(peer, channel, MakePacket(channel, data));
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

bool NetClient::Connect(const std::string& host, uint16_t port, uint32_t timeoutMs, std::string& error) {
    EnsureEnetInitialized();
    if (!impl->host) impl->host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
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
    impl->peer = enet_host_connect(impl->host, &address, kChannelCount, 0);
    if (!impl->peer) {
        error = "no free peer slot";
        return false;
    }

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    ENetEvent e;
    while (Clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (enet_host_service(impl->host, &e, static_cast<enet_uint32>(std::max<long long>(1, remaining))) <= 0) continue;
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
    if (impl->connected) enet_peer_send(impl->peer, channel, MakePacket(channel, data));
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
