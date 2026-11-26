#include "net/RelayHost.h"

#include <chrono>
#include <cstdlib>
#include <random>
#include <unordered_map>
#include <vector>

#include <enet/enet.h>

#include "relay/RelayProtocol.h"

namespace net {

namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void SendBytes(ENetSocket socket, const ENetAddress& to, const uint8_t* data, size_t size) {
    ENetBuffer buffer;
    buffer.data = const_cast<uint8_t*>(data);
    buffer.dataLength = size;
    enet_socket_send(socket, &to, &buffer, 1);
}

int Receive(ENetSocket socket, ENetAddress& from, uint8_t* data, size_t size) {
    ENetBuffer buffer;
    buffer.data = data;
    buffer.dataLength = size;
    return enet_socket_receive(socket, &from, &buffer, 1);
}

ENetSocket OpenSocket() {
    ENetSocket socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (socket == ENET_SOCKET_NULL) return socket;
    ENetAddress any;
    any.host = ENET_HOST_ANY;
    any.port = 0;
    enet_socket_bind(socket, &any);
    enet_socket_set_option(socket, ENET_SOCKOPT_NONBLOCK, 1);
    enet_socket_set_option(socket, ENET_SOCKOPT_RCVBUF, 1024 * 1024);
    enet_socket_set_option(socket, ENET_SOCKOPT_SNDBUF, 1024 * 1024);
    return socket;
}

} // namespace

bool SplitAddress(const std::string& text, uint16_t defaultPort, std::string& host, uint16_t& port) {
    host = text;
    port = defaultPort;
    while (!host.empty() && host.back() == ' ') host.pop_back();
    while (!host.empty() && host.front() == ' ') host.erase(host.begin());
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) {
        port = static_cast<uint16_t>(std::atoi(host.c_str() + colon + 1));
        host.resize(colon);
    }
    return !host.empty() && port != 0;
}

RelayHost::~RelayHost() {
    Stop();
}

bool RelayHost::Start(const std::string& relay, uint16_t serverPort, std::string& error) {
    Stop();
    std::string host;
    uint16_t port;
    if (!SplitAddress(relay, relay::kDefaultPort, host, port)) {
        error = "no relay server is set (Settings)";
        return false;
    }
    if (enet_initialize() != 0) { // (counted: safe to call again)
        error = "couldn't start networking";
        return false;
    }
    ENetAddress address;
    const bool resolved = enet_address_set_host(&address, host.c_str()) == 0;
    enet_deinitialize();
    if (!resolved) {
        error = "can't find the relay '" + host + "'";
        return false;
    }
    relayHost = address.host;
    relayPort = port;
    relayName = host;
    localPort = serverPort;
    state = State::Opening;
    code.clear();
    refusal.clear();
    stopping = false;
    thread = std::thread([this] { Run(); });
    return true;
}

void RelayHost::Stop() {
    if (!thread.joinable()) return;
    stopping = true;
    thread.join();
    friends = 0;
}

RelayHost::State RelayHost::GetState() const {
    std::lock_guard<std::mutex> lock(mutex);
    return state;
}

std::string RelayHost::GetCode() const {
    std::lock_guard<std::mutex> lock(mutex);
    return code;
}

std::string RelayHost::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex);
    switch (state) {
    case State::Opening: return "Asking the relay (" + relayName + ") for a code...";
    case State::Refused: return "The relay said: " + refusal;
    case State::Open: break;
    }
    const int n = friends.load();
    return n == 0 ? "No one has joined yet" : n == 1 ? "1 friend playing" : std::to_string(n) + " friends playing";
}

void RelayHost::Run() {
    enet_initialize();
    ENetAddress relayAddress;
    relayAddress.host = relayHost;
    relayAddress.port = relayPort;
    ENetAddress server;
    enet_address_set_host_ip(&server, "127.0.0.1");
    server.port = localPort;

    std::mt19937_64 random{ std::random_device{}() ^ static_cast<uint64_t>(Now() * 1e6) };
    uint64_t secret = 0;
    while (secret == 0) secret = random();

    ENetSocket relaySocket = OpenSocket();
    struct Friend {
        ENetSocket socket;
        double last;
    };
    std::unordered_map<uint32_t, Friend> joiners;
    auto forget = [&](uint32_t id) {
        auto it = joiners.find(id);
        if (it == joiners.end()) return;
        enet_socket_destroy(it->second.socket);
        joiners.erase(it);
    };

    std::vector<uint8_t> buffer(relay::kMaxDatagram);
    double lastOpen = -100.0;
    std::string myCode;
    while (!stopping && relaySocket != ENET_SOCKET_NULL) {
        const double now = Now();
        // Ask for a room until there is one, then say we're still here every few seconds
        // (which also keeps this machine's router passing the relay's datagrams in).
        State current;
        {
            std::lock_guard<std::mutex> lock(mutex);
            current = state;
        }
        const double every = current == State::Open ? 5.0 : current == State::Refused ? 10.0 : 1.0;
        if (now - lastOpen > every) {
            lastOpen = now;
            const auto open = relay::Writer(relay::Msg::HostOpen).U64(secret).Str(myCode).bytes;
            SendBytes(relaySocket, relayAddress, open.data(), open.size());
        }

        ENetSocketSet read;
        ENET_SOCKETSET_EMPTY(read);
        ENET_SOCKETSET_ADD(read, relaySocket);
        ENetSocket highest = relaySocket;
        for (const auto& [id, f] : joiners) {
            ENET_SOCKETSET_ADD(read, f.socket);
            if (f.socket > highest) highest = f.socket;
        }
        if (enet_socketset_select(highest, &read, nullptr, 50) <= 0) {
            for (auto it = joiners.begin(); it != joiners.end();) {
                if (now - it->second.last > 90.0) {
                    enet_socket_destroy(it->second.socket);
                    it = joiners.erase(it);
                }
                else ++it;
            }
            friends = static_cast<int>(joiners.size());
            continue;
        }

        // From the relay: friends' datagrams (for the server), and answers.
        ENetAddress from;
        for (int n; (n = Receive(relaySocket, from, buffer.data(), buffer.size())) > 0;) {
            const uint8_t* data = buffer.data();
            const size_t size = static_cast<size_t>(n);
            if (from.host != relayAddress.host || from.port != relayAddress.port || !relay::IsControl(data, size)) continue;
            relay::Reader r(data, size);
            switch (relay::TypeOf(data)) {
            case relay::Msg::HostOk: {
                std::string got;
                if (!r.Str(got)) break;
                myCode = got;
                std::lock_guard<std::mutex> lock(mutex);
                state = State::Open;
                code = got;
                break;
            }
            case relay::Msg::Refused: {
                std::lock_guard<std::mutex> lock(mutex);
                state = State::Refused;
                refusal.assign(reinterpret_cast<const char*>(r.p), r.left);
                break;
            }
            case relay::Msg::JoinerData: {
                uint32_t id;
                if (!r.U32(id) || r.left == 0) break;
                auto it = joiners.find(id);
                if (it == joiners.end()) {
                    if (joiners.size() >= 60) break; // (more than a socket set holds)
                    const ENetSocket socket = OpenSocket();
                    if (socket == ENET_SOCKET_NULL) break;
                    it = joiners.emplace(id, Friend{ socket, Now() }).first;
                }
                it->second.last = Now();
                SendBytes(it->second.socket, server, r.p, r.left);
                break;
            }
            case relay::Msg::JoinerGone: {
                uint32_t id;
                if (r.U32(id)) forget(id);
                break;
            }
            default: break;
            }
        }
        // From the server, for each friend.
        for (auto& [id, f] : joiners) {
            if (!ENET_SOCKETSET_CHECK(read, f.socket)) continue;
            for (int n; (n = Receive(f.socket, from, buffer.data() + 9, buffer.size() - 9)) > 0;) {
                if (from.port != server.port) continue;
                f.last = Now();
                auto out = relay::Writer(relay::Msg::HostData).U32(id).bytes;
                out.insert(out.end(), buffer.data() + 9, buffer.data() + 9 + n);
                SendBytes(relaySocket, relayAddress, out.data(), out.size());
            }
        }
        friends = static_cast<int>(joiners.size());
    }

    if (relaySocket != ENET_SOCKET_NULL) {
        const auto close = relay::Writer(relay::Msg::HostClose).U64(secret).bytes;
        SendBytes(relaySocket, relayAddress, close.data(), close.size());
        enet_socket_destroy(relaySocket);
    }
    for (auto& [id, f] : joiners) enet_socket_destroy(f.socket);
    enet_deinitialize();
}

} // namespace net
