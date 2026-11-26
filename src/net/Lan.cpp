#include "net/Lan.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <unordered_map>

#include "net/Protocol.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kNoSocket = -1;
#endif

namespace net {

namespace {

// An announcement: magic, version, flags, the game's id (a random number, so one
// heard on two networks is listed once), port, protocol, players, then the host's and
// the world's names (a length byte before each).
constexpr uint8_t kMagic[4] = { 'V', 'X', 'L', 'N' };
constexpr uint8_t kVersion = 1;
constexpr uint8_t kGoodbye = 1;              // flags: the game has closed
constexpr double kAnnounceSeconds = 1.0;
constexpr double kForgetSeconds = 4.0;       // a game not heard from this long has gone
constexpr double kPlayerIdleSeconds = 90.0;  // a forwarded player silent this long has gone

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool StartSockets() {
#ifdef _WIN32
    static const bool started = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return started;
#else
    return true;
#endif
}

void CloseSocket(Socket s) {
    if (s == kNoSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

void SetNonBlocking(Socket s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
    // (Don't report a datagram that bounced as an error on the next receive.)
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &report, sizeof report, nullptr, 0, &returned, nullptr, nullptr);
#else
    (void)s;
#endif
}

sockaddr_in Address(uint32_t hostOrderIp, uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(hostOrderIp);
    a.sin_port = htons(port);
    return a;
}

Socket OpenUdp(uint32_t bindIp, uint16_t bindPort, bool reuse = false) {
    Socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return s;
    if (reuse) {
        const int on = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
#ifdef SO_REUSEPORT
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&on), sizeof on);
#endif
    }
    const sockaddr_in a = Address(bindIp, bindPort);
    if (bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
        CloseSocket(s);
        return kNoSocket;
    }
    SetNonBlocking(s);
    return s;
}

uint16_t PortOf(Socket s) {
    sockaddr_in a{};
    socklen_t length = sizeof a;
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &length);
    return ntohs(a.sin_port);
}

int ReceiveFrom(Socket s, uint8_t* data, size_t size, sockaddr_in& from) {
    socklen_t length = sizeof from;
    return static_cast<int>(recvfrom(s, reinterpret_cast<char*>(data), static_cast<int>(size), 0, reinterpret_cast<sockaddr*>(&from), &length));
}

void SendTo(Socket s, const uint8_t* data, size_t size, const sockaddr_in& to) {
    sendto(s, reinterpret_cast<const char*>(data), static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
}

uint64_t KeyOf(const sockaddr_in& a) {
    return (static_cast<uint64_t>(ntohl(a.sin_addr.s_addr)) << 16) | ntohs(a.sin_port);
}

std::string Dotted(uint32_t hostOrderIp) {
    return std::to_string(hostOrderIp >> 24) + "." + std::to_string((hostOrderIp >> 16) & 255) + "." +
           std::to_string((hostOrderIp >> 8) & 255) + "." + std::to_string(hostOrderIp & 255);
}

struct Interface {
    uint32_t ip = 0, broadcast = 0; // host order
    bool virtualAdapter = false;    // (a VM's or WSL's: not worth showing players)
};

std::vector<Interface> Interfaces() {
    std::vector<Interface> found;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && result == ERROR_BUFFER_OVERFLOW; tries++) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result != NO_ERROR) return found;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        const std::wstring friendly = a->FriendlyName ? a->FriendlyName : L"";
        const std::wstring description = a->Description ? a->Description : L"";
        const bool virtualAdapter = friendly.rfind(L"vEthernet", 0) == 0 || description.find(L"Virtual") != std::wstring::npos ||
                                    description.find(L"VMware") != std::wstring::npos || description.find(L"Hyper-V") != std::wstring::npos;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
            const ULONG prefix = std::min<ULONG>(u->OnLinkPrefixLength, 32);
            const uint32_t mask = prefix == 0 ? 0u : ~0u << (32 - prefix);
            found.push_back({ ip, ip | ~mask, virtualAdapter });
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return found;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        Interface f;
        f.ip = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
        f.broadcast = (i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr ? ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_broadaddr)->sin_addr.s_addr)
                                                                         : 0xFFFFFFFFu;
        const std::string name = i->ifa_name ? i->ifa_name : "";
        f.virtualAdapter = name.rfind("docker", 0) == 0 || name.rfind("veth", 0) == 0 || name.rfind("br-", 0) == 0 || name.rfind("virbr", 0) == 0;
        found.push_back(f);
    }
    freeifaddrs(list);
#endif
    return found;
}

std::vector<uint8_t> Announcement(uint8_t flags, uint64_t id, uint16_t port, uint8_t players, const std::string& name, const std::string& world) {
    std::vector<uint8_t> b(kMagic, kMagic + 4);
    b.push_back(kVersion);
    b.push_back(flags);
    for (int i = 0; i < 8; i++) b.push_back(static_cast<uint8_t>(id >> (8 * i)));
    b.push_back(static_cast<uint8_t>(port));
    b.push_back(static_cast<uint8_t>(port >> 8));
    for (int i = 0; i < 4; i++) b.push_back(static_cast<uint8_t>(kProtocolVersion >> (8 * i)));
    b.push_back(players);
    for (const std::string* s : { &name, &world }) {
        const size_t n = std::min<size_t>(s->size(), 64);
        b.push_back(static_cast<uint8_t>(n));
        b.insert(b.end(), s->begin(), s->begin() + static_cast<std::ptrdiff_t>(n));
    }
    return b;
}

} // namespace

std::vector<std::string> LocalAddresses() {
    std::vector<std::string> addresses, virtualOnes;
    if (!StartSockets()) return addresses;
    for (const Interface& i : Interfaces()) {
        if ((i.ip >> 16) == 0xA9FE) continue; // (169.254: no network's given it an address)
        (i.virtualAdapter ? virtualOnes : addresses).push_back(Dotted(i.ip));
    }
    return addresses.empty() ? virtualOnes : addresses;
}

// --- LanHost ----------------------------------------------------------------------

LanHost::~LanHost() {
    Stop();
}

bool LanHost::Start(const std::string& hostName, const std::string& worldName, uint16_t gameServerPort, bool forwardPlayers,
                    std::string& error, uint16_t announceTo) {
    Stop();
    if (!StartSockets()) {
        error = "couldn't start networking";
        return false;
    }
    name = hostName;
    world = worldName;
    serverPort = gameServerPort;
    forward = forwardPlayers;
    discoveryPort = announceTo;
    port = serverPort;
    if (forward) {
        // The game's usual port, so players can also type just the address; else any.
        const uint32_t on = loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY;
        Socket s = OpenUdp(on, kDefaultPort);
        if (s == kNoSocket) s = OpenUdp(on, 0);
        if (s == kNoSocket) {
            error = "couldn't listen on the network";
            return false;
        }
        const int buffer = 1024 * 1024;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof buffer);
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof buffer);
        port = PortOf(s);
        listenSocket = static_cast<uint64_t>(s);
    }
    stopping = false;
    thread = std::thread([this] { Run(); });
    return true;
}

void LanHost::Stop() {
    if (!thread.joinable()) return;
    stopping = true;
    thread.join();
    players = 0;
}

std::vector<std::string> LanHost::GetAddresses() const {
    return LocalAddresses();
}

void LanHost::Run() {
    const Socket listen = forward ? static_cast<Socket>(listenSocket) : kNoSocket;
    Socket announce = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (announce != kNoSocket) {
        const int on = 1;
        setsockopt(announce, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
        SetNonBlocking(announce);
    }
    std::mt19937_64 random{ std::random_device{}() ^ static_cast<uint64_t>(Now() * 1e6) };
    const uint64_t id = random();
    const sockaddr_in server = Address(INADDR_LOOPBACK, serverPort);

    // Where announcements go: each network's broadcast address (looked at again now and
    // then: Wi-Fi comes and goes), and this machine itself.
    std::vector<sockaddr_in> targets;
    double lastLooked = -100.0, lastAnnounced = -100.0;
    auto announceTo = [&](uint8_t flags) {
        if (announce == kNoSocket) return;
        const auto message = Announcement(flags, id, port, static_cast<uint8_t>(forward ? std::min(254, players.load() + 1) : 255), name, world);
        for (const sockaddr_in& to : targets) SendTo(announce, message.data(), message.size(), to);
    };

    struct Player {
        Socket socket;
        sockaddr_in address;
        double last;
    };
    std::unordered_map<uint64_t, Player> joined; // by their address
    std::vector<uint8_t> buffer(4096);
    while (!stopping) {
        const double now = Now();
        if (now - lastLooked > 10.0) {
            lastLooked = now;
            targets.clear();
            targets.push_back(Address(INADDR_LOOPBACK, discoveryPort));
            if (!loopbackOnly)
                for (const Interface& i : Interfaces())
                    if (i.broadcast) targets.push_back(Address(i.broadcast, discoveryPort));
        }
        if (now - lastAnnounced > kAnnounceSeconds) {
            lastAnnounced = now;
            announceTo(0);
        }
        if (!forward) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        fd_set read;
        FD_ZERO(&read);
        FD_SET(listen, &read);
        Socket highest = listen;
        for (const auto& [key, p] : joined) {
            FD_SET(p.socket, &read);
            highest = std::max(highest, p.socket);
        }
        timeval wait{ 0, 50000 };
        if (select(static_cast<int>(highest) + 1, &read, nullptr, nullptr, &wait) <= 0) {
            for (auto it = joined.begin(); it != joined.end();) {
                if (now - it->second.last > kPlayerIdleSeconds) {
                    CloseSocket(it->second.socket);
                    it = joined.erase(it);
                }
                else ++it;
            }
            players = static_cast<int>(joined.size());
            continue;
        }
        // From players on the network: to the server, each over a socket of its own.
        if (FD_ISSET(listen, &read)) {
            sockaddr_in from{};
            for (int n; (n = ReceiveFrom(listen, buffer.data(), buffer.size(), from)) > 0;) {
                auto it = joined.find(KeyOf(from));
                if (it == joined.end()) {
                    if (joined.size() >= 60) continue; // (more than a socket set holds)
                    const Socket s = OpenUdp(INADDR_LOOPBACK, 0);
                    if (s == kNoSocket) continue;
                    it = joined.emplace(KeyOf(from), Player{ s, from, Now() }).first;
                }
                it->second.last = Now();
                SendTo(it->second.socket, buffer.data(), static_cast<size_t>(n), server);
            }
        }
        // From the server: back to each player.
        for (auto& [key, p] : joined) {
            if (!FD_ISSET(p.socket, &read)) continue;
            sockaddr_in from{};
            for (int n; (n = ReceiveFrom(p.socket, buffer.data(), buffer.size(), from)) > 0;) {
                p.last = Now();
                SendTo(listen, buffer.data(), static_cast<size_t>(n), p.address);
            }
        }
        players = static_cast<int>(joined.size());
    }

    announceTo(kGoodbye);
    for (auto& [key, p] : joined) CloseSocket(p.socket);
    CloseSocket(announce);
    CloseSocket(listen);
    listenSocket = ~0ull;
}

// --- LanBrowser -------------------------------------------------------------------

LanBrowser::~LanBrowser() {
    Stop();
}

bool LanBrowser::Start(std::string& error, uint16_t discoveryPort) {
    Stop();
    if (!StartSockets()) {
        error = "couldn't start networking";
        return false;
    }
    const Socket s = OpenUdp(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY, discoveryPort, true);
    if (s == kNoSocket) {
        error = "couldn't listen for games on the network";
        return false;
    }
    socket = static_cast<uint64_t>(s);
    stopping = false;
    thread = std::thread([this] { Run(); });
    return true;
}

void LanBrowser::Stop() {
    if (!thread.joinable()) return;
    stopping = true;
    thread.join();
    std::lock_guard<std::mutex> lock(mutex);
    heard.clear();
}

std::vector<LanGame> LanBrowser::GetGames() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<LanGame> games;
    const double now = Now();
    for (const Heard& h : heard)
        if (now - h.last < kForgetSeconds) games.push_back(h.game);
    return games;
}

void LanBrowser::Run() {
    const Socket s = static_cast<Socket>(socket);
    uint8_t buffer[512];
    while (!stopping) {
        fd_set read;
        FD_ZERO(&read);
        FD_SET(s, &read);
        timeval wait{ 0, 100000 };
        if (select(static_cast<int>(s) + 1, &read, nullptr, nullptr, &wait) <= 0) continue;
        sockaddr_in from{};
        for (int n; (n = ReceiveFrom(s, buffer, sizeof buffer, from)) > 0;) {
            const size_t size = static_cast<size_t>(n);
            if (size < 21 || std::memcmp(buffer, kMagic, 4) != 0 || buffer[4] != kVersion) continue;
            const uint8_t flags = buffer[5];
            uint64_t id = 0;
            for (int i = 0; i < 8; i++) id |= static_cast<uint64_t>(buffer[6 + i]) << (8 * i);
            LanGame game;
            game.port = static_cast<uint16_t>(buffer[14] | (buffer[15] << 8));
            uint32_t protocol = 0;
            for (int i = 0; i < 4; i++) protocol |= static_cast<uint32_t>(buffer[16 + i]) << (8 * i);
            game.compatible = protocol == kProtocolVersion;
            game.players = buffer[20] == 255 ? -1 : buffer[20];
            size_t at = 21;
            bool ok = true;
            for (std::string* text : { &game.name, &game.world }) {
                if (at >= size || at + 1 + buffer[at] > size) {
                    ok = false;
                    break;
                }
                text->assign(reinterpret_cast<const char*>(buffer + at + 1), buffer[at]);
                at += 1 + buffer[at];
            }
            if (!ok) continue;
            game.address = Dotted(ntohl(from.sin_addr.s_addr));

            std::lock_guard<std::mutex> lock(mutex);
            auto it = std::find_if(heard.begin(), heard.end(), [&](const Heard& h) { return h.id == id; });
            if (flags & kGoodbye) {
                if (it != heard.end()) heard.erase(it);
                continue;
            }
            if (it == heard.end()) heard.push_back({ id, game, Now() });
            else {
                // Heard on more than one network: keep the address first heard from, unless
                // it's from this machine itself (then 127.0.0.1 always works).
                if (game.address != "127.0.0.1" && it->game.address != game.address && Now() - it->last < kForgetSeconds) game.address = it->game.address;
                it->game = game;
                it->last = Now();
            }
        }
    }
    CloseSocket(s);
    socket = ~0ull;
}

} // namespace net
