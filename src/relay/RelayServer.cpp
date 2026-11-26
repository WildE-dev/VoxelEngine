#include "relay/RelayServer.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <iterator>
#include <random>
#include <unordered_map>
#include <vector>

#include "relay/RelayProtocol.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kNoSocket = -1;
#endif

namespace relay {

namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// An address and port as one number (IPv4).
uint64_t KeyOf(const sockaddr_in& a) {
    return (static_cast<uint64_t>(ntohl(a.sin_addr.s_addr)) << 16) | ntohs(a.sin_port);
}

uint32_t HostOf(const sockaddr_in& a) { return ntohl(a.sin_addr.s_addr); }

const char* const kFirst[] = {
    "amber", "brave", "calm", "dusty", "eager", "fuzzy", "gentle", "happy", "icy", "jolly", "keen", "lucky",
    "mellow", "nimble", "olive", "proud", "quiet", "rusty", "sandy", "tidy", "urban", "vivid", "witty", "young",
    "bold", "crisp", "deep", "early", "fancy", "grand", "hidden", "iron", "jade", "kind", "lofty", "misty",
    "noble", "odd", "plain", "quick", "royal", "silver", "tall", "upper", "velvet", "wild", "golden", "stony",
};
const char* const kSecond[] = {
    "otter", "badger", "cliff", "dune", "eagle", "fern", "goat", "heron", "island", "jay", "kettle", "lantern",
    "meadow", "newt", "orchard", "pine", "quarry", "river", "stone", "thistle", "valley", "willow", "yak", "zebra",
    "anvil", "bridge", "cavern", "dwarf", "ember", "forge", "glade", "hollow", "ingot", "juniper", "knoll", "ledge",
    "mole", "nugget", "oak", "pebble", "quill", "ridge", "summit", "torch", "tunnel", "vault", "wolf", "brook",
};

} // namespace

struct RelayServer::Impl {
    struct Joiner {
        sockaddr_in address{};
        double last = 0.0;
    };
    struct Room {
        std::string code;
        uint64_t secret = 0;
        sockaddr_in host{};
        double last = 0.0;
        uint32_t nextJoiner = 1;
        std::unordered_map<uint32_t, Joiner> joiners;
    };
    struct Attempts {
        int count = 0;
        double since = 0.0;
    };

    RelayConfig config;
    Socket socket = kNoSocket;
    uint16_t port = 0;
    std::mt19937_64 random{ std::random_device{}() ^ static_cast<uint64_t>(std::time(nullptr)) };
    std::unordered_map<std::string, Room> rooms;              // by code
    std::unordered_map<uint64_t, std::string> bySecret;       // secret -> code
    std::unordered_map<uint64_t, std::string> byHost;         // host address -> code
    std::unordered_map<uint64_t, std::pair<std::string, uint32_t>> byJoiner; // joiner address -> code, number
    std::unordered_map<uint32_t, Attempts> attempts;          // joins tried, by address
    double lastSweep = 0.0;

    void Log(const std::string& line) const {
        if (config.log) std::cout << "[relay] " << line << std::endl;
    }

    void Send(const sockaddr_in& to, const std::vector<uint8_t>& bytes) {
        sendto(socket, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
               reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }
    void SendRaw(const sockaddr_in& to, const uint8_t* data, size_t size) {
        sendto(socket, reinterpret_cast<const char*>(data), static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }
    void Refuse(const sockaddr_in& to, const std::string& why) {
        Writer w(Msg::Refused);
        w.Raw(reinterpret_cast<const uint8_t*>(why.data()), why.size());
        Send(to, w.bytes);
    }

    std::string NewCode() {
        for (;;) {
            char code[kMaxCode + 1];
            std::snprintf(code, sizeof code, "%s-%s-%d", kFirst[random() % std::size(kFirst)], kSecond[random() % std::size(kSecond)],
                          static_cast<int>(random() % 90) + 10);
            if (!rooms.count(code)) return code;
        }
    }

    void Close(const std::string& code, const char* why) {
        auto it = rooms.find(code);
        if (it == rooms.end()) return;
        for (const auto& [id, joiner] : it->second.joiners) byJoiner.erase(KeyOf(joiner.address));
        bySecret.erase(it->second.secret);
        byHost.erase(KeyOf(it->second.host));
        rooms.erase(it);
        Log("closed " + code + " (" + why + "; " + std::to_string(rooms.size()) + " open)");
    }

    void HostOpen(const sockaddr_in& from, const uint8_t* data, size_t size) {
        Reader r(data, size);
        uint64_t secret;
        std::string wanted;
        if (!r.U64(secret) || !r.Str(wanted) || secret == 0) return;
        const double now = Now();
        if (auto known = bySecret.find(secret); known != bySecret.end()) {
            Room& room = rooms[known->second];
            if (KeyOf(room.host) != KeyOf(from)) { // (the host's router gave it a new port)
                byHost.erase(KeyOf(room.host));
                room.host = from;
                byHost[KeyOf(from)] = room.code;
            }
            room.last = now;
            Send(from, Writer(Msg::HostOk).Str(room.code).bytes);
            return;
        }
        if (static_cast<int>(rooms.size()) >= config.maxRooms) return Refuse(from, "The relay is full; try again later");
        int fromHere = 0;
        for (const auto& [code, room] : rooms) fromHere += HostOf(room.host) == HostOf(from);
        if (fromHere >= config.maxRoomsPerAddress) return Refuse(from, "Too many games open from here");
        if (auto old = byHost.find(KeyOf(from)); old != byHost.end()) Close(old->second, "replaced");
        wanted = NormalizeCode(wanted);
        Room room;
        room.code = LooksLikeCode(wanted) && !rooms.count(wanted) ? wanted : NewCode();
        room.secret = secret;
        room.host = from;
        room.last = now;
        bySecret[secret] = room.code;
        byHost[KeyOf(from)] = room.code;
        Send(from, Writer(Msg::HostOk).Str(room.code).bytes);
        rooms[room.code] = room;
        Log("opened " + room.code + " (" + std::to_string(rooms.size()) + " open)");
    }

    void HostData(const sockaddr_in& from, const uint8_t* data, size_t size) {
        auto host = byHost.find(KeyOf(from));
        if (host == byHost.end()) return;
        Room& room = rooms[host->second];
        room.last = Now();
        Reader r(data, size);
        uint32_t id;
        if (!r.U32(id)) return;
        auto joiner = room.joiners.find(id);
        if (joiner != room.joiners.end() && r.left > 0) SendRaw(joiner->second.address, r.p, r.left);
    }

    void HostClose(const sockaddr_in& from, const uint8_t* data, size_t size) {
        Reader r(data, size);
        uint64_t secret;
        if (!r.U64(secret)) return;
        auto known = bySecret.find(secret);
        if (known != bySecret.end() && KeyOf(rooms[known->second].host) == KeyOf(from)) Close(known->second, "the host closed it");
    }

    void Join(const sockaddr_in& from, const uint8_t* data, size_t size) {
        if (byJoiner.count(KeyOf(from))) { // (a repeat: the answer went missing)
            Send(from, Writer(Msg::JoinOk).bytes);
            return;
        }
        const double now = Now();
        Attempts& tries = attempts[HostOf(from)];
        if (now - tries.since > 60.0) tries = { 0, now };
        if (++tries.count > config.joinAttemptsPerMinute) return Refuse(from, "Too many tries; wait a minute");
        Reader r(data, size);
        std::string code;
        if (!r.Str(code)) return;
        auto it = rooms.find(NormalizeCode(code));
        if (it == rooms.end()) return Refuse(from, "No game is open with that code");
        Room& room = it->second;
        if (static_cast<int>(room.joiners.size()) >= config.maxJoinersPerRoom) return Refuse(from, "That game is full");
        const uint32_t id = room.nextJoiner++;
        room.joiners[id] = { from, now };
        byJoiner[KeyOf(from)] = { room.code, id };
        Send(from, Writer(Msg::JoinOk).bytes);
    }

    void FromJoiner(const sockaddr_in& from, const uint8_t* data, size_t size) {
        auto it = byJoiner.find(KeyOf(from));
        if (it == byJoiner.end()) return;
        Room& room = rooms[it->second.first];
        room.joiners[it->second.second].last = Now();
        Send(room.host, Writer(Msg::JoinerData).U32(it->second.second).Raw(data, size).bytes);
    }

    void Sweep() {
        const double now = Now();
        std::vector<std::string> silent;
        for (auto& [code, room] : rooms) {
            if (now - room.last > config.hostTimeoutSeconds) {
                silent.push_back(code);
                continue;
            }
            for (auto it = room.joiners.begin(); it != room.joiners.end();) {
                if (now - it->second.last > config.joinerTimeoutSeconds) {
                    Send(room.host, Writer(Msg::JoinerGone).U32(it->first).bytes);
                    byJoiner.erase(KeyOf(it->second.address));
                    it = room.joiners.erase(it);
                }
                else ++it;
            }
        }
        for (const std::string& code : silent) Close(code, "the host went quiet");
        for (auto it = attempts.begin(); it != attempts.end();) it = now - it->second.since > 60.0 ? attempts.erase(it) : std::next(it);
    }
};

RelayServer::RelayServer() : impl(std::make_unique<Impl>()) {}

RelayServer::~RelayServer() {
    if (impl->socket == kNoSocket) return;
#ifdef _WIN32
    closesocket(impl->socket);
#else
    close(impl->socket);
#endif
}

bool RelayServer::Start(const RelayConfig& config, std::string& error) {
#ifdef _WIN32
    static const bool started = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    if (!started) {
        error = "couldn't start Winsock";
        return false;
    }
#endif
    impl->config = config;
    impl->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl->socket == kNoSocket) {
        error = "couldn't make a socket";
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    if (inet_pton(AF_INET, config.bindHost.c_str(), &address.sin_addr) != 1) {
        error = "'" + config.bindHost + "' isn't an IPv4 address";
        return false;
    }
    if (bind(impl->socket, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        error = "couldn't listen on port " + std::to_string(config.port) + " (in use?)";
        return false;
    }
    socklen_t length = sizeof address;
    getsockname(impl->socket, reinterpret_cast<sockaddr*>(&address), &length);
    impl->port = ntohs(address.sin_port);
#ifdef _WIN32
    // (Windows reports a datagram that bounced, "port unreachable", as an error on the
    // next receive; a host or joiner that's gone isn't the relay's problem.)
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(impl->socket, _WSAIOW(IOC_VENDOR, 12), &report, sizeof report, nullptr, 0, &returned, nullptr, nullptr);
#endif
    const int buffer = 4 * 1024 * 1024;
    setsockopt(impl->socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof buffer);
    setsockopt(impl->socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof buffer);
    impl->Log("listening on UDP port " + std::to_string(impl->port));
    return true;
}

uint16_t RelayServer::GetPort() const {
    return impl->port;
}

int RelayServer::RoomCount() const {
    return static_cast<int>(impl->rooms.size());
}

void RelayServer::RunOnce(int timeoutMs) {
    Impl& s = *impl;
    const double end = Now() + timeoutMs / 1000.0;
    uint8_t buffer[kMaxDatagram];
    for (;;) {
        const double left = end - Now();
        fd_set read;
        FD_ZERO(&read);
        FD_SET(s.socket, &read);
        timeval wait{};
        if (left > 0.0) {
            wait.tv_sec = static_cast<long>(left);
            wait.tv_usec = static_cast<long>((left - static_cast<double>(wait.tv_sec)) * 1e6);
        }
        const int ready = select(static_cast<int>(s.socket) + 1, &read, nullptr, nullptr, &wait);
        if (ready > 0) {
            sockaddr_in from{};
            socklen_t length = sizeof from;
            const int n = static_cast<int>(recvfrom(s.socket, reinterpret_cast<char*>(buffer), sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &length));
            if (n > 0 && from.sin_family == AF_INET) {
                const size_t size = static_cast<size_t>(n);
                if (!IsControl(buffer, size)) s.FromJoiner(from, buffer, size);
                else switch (TypeOf(buffer)) {
                    case Msg::HostOpen: s.HostOpen(from, buffer, size); break;
                    case Msg::HostData: s.HostData(from, buffer, size); break;
                    case Msg::HostClose: s.HostClose(from, buffer, size); break;
                    case Msg::Join: s.Join(from, buffer, size); break;
                    default: break;
                    }
            }
        }
        if (Now() - s.lastSweep > 1.0) {
            s.lastSweep = Now();
            s.Sweep();
        }
        if (Now() >= end) return;
    }
}

} // namespace relay
