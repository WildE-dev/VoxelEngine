#pragma once
// The relay's wire format (shared by the relay, VoxelRelay, and the game). See README.md,
// "Playing with friends over the internet".
//
// Everything is UDP to the relay's one port. A host (a game opened to friends) sends
// HostOpen and gets a join code back; a friend's game sends Join with that code from the
// socket it will play on, then talks ENet to the relay as if it were the server. The
// relay passes each friend's datagrams to the host wrapped in JoinerData (with the
// friend's number), and the host's replies back the other way in HostData.
//
// Control messages start with kMagic and a message type. A friend's ENet datagrams are
// passed on as they are: ENet's header never starts with the magic (its first two bytes
// are a peer number under 4096, or 0x0FFF to connect, with flags).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace relay {

constexpr uint16_t kDefaultPort = 25700;
constexpr uint8_t kMagic[4] = { 'V', 'X', 'R', '1' };
constexpr size_t kHeader = 5;          // magic and type
constexpr size_t kMaxDatagram = 2048;  // (ENet's are at most ~1400 bytes)
constexpr size_t kMaxCode = 32;

enum class Msg : uint8_t {
    // host -> relay: [u64 secret][u8 length][code]. Opens a room, or (the same secret
    // again) keeps it open, from wherever the host now is. The code asks for that one
    // back (after the relay restarted, say); empty: any.
    HostOpen = 1,
    HostOk = 2,      // relay -> host: [u8 length][code]
    HostData = 3,    // host -> relay: [u32 joiner][datagram], passed to that joiner
    HostClose = 4,   // host -> relay: [u64 secret]
    JoinerData = 5,  // relay -> host: [u32 joiner][datagram] from that joiner
    JoinerGone = 6,  // relay -> host: [u32 joiner] (gone quiet)
    Join = 7,        // joiner -> relay: [u8 length][code]
    JoinOk = 8,      // relay -> joiner
    Refused = 9,     // relay -> either: [text] why (no such code, full, too many)
};

inline bool IsControl(const uint8_t* data, size_t size) {
    return size >= kHeader && std::memcmp(data, kMagic, 4) == 0;
}

// A message: the header, then whatever is appended.
struct Writer {
    std::vector<uint8_t> bytes;
    explicit Writer(Msg type) : bytes(kMagic, kMagic + 4) { bytes.push_back(static_cast<uint8_t>(type)); }
    Writer& U32(uint32_t v) {
        for (int i = 0; i < 4; i++) bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
        return *this;
    }
    Writer& U64(uint64_t v) {
        for (int i = 0; i < 8; i++) bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
        return *this;
    }
    Writer& Str(const std::string& s) {
        const size_t n = s.size() < 255 ? s.size() : 255;
        bytes.push_back(static_cast<uint8_t>(n));
        bytes.insert(bytes.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
        return *this;
    }
    Writer& Raw(const uint8_t* data, size_t size) {
        bytes.insert(bytes.end(), data, data + size);
        return *this;
    }
};

struct Reader {
    const uint8_t* p;
    size_t left;
    Reader(const uint8_t* data, size_t size) : p(data + kHeader), left(size - kHeader) {}
    bool U32(uint32_t& v) {
        if (left < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(p[i]) << (8 * i);
        p += 4, left -= 4;
        return true;
    }
    bool U64(uint64_t& v) {
        if (left < 8) return false;
        v = 0;
        for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(p[i]) << (8 * i);
        p += 8, left -= 8;
        return true;
    }
    bool Str(std::string& s) {
        if (left < 1 || left - 1 < p[0]) return false;
        s.assign(reinterpret_cast<const char*>(p + 1), p[0]);
        left -= 1 + p[0], p += 1 + p[0];
        return true;
    }
};

inline Msg TypeOf(const uint8_t* data) { return static_cast<Msg>(data[4]); }

// A join code as typed: trimmed, lower case, spaces as dashes ("Amber Otter 42" is
// "amber-otter-42").
inline std::string NormalizeCode(const std::string& typed) {
    std::string code;
    for (char c : typed) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == ' ' || c == '_') c = '-';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') code += c;
    }
    while (!code.empty() && code.front() == '-') code.erase(code.begin());
    while (!code.empty() && code.back() == '-') code.pop_back();
    return code.size() <= kMaxCode ? code : std::string();
}

// Whether text looks like a join code (word-word-number) rather than an address.
inline bool LooksLikeCode(const std::string& typed) {
    if (typed.find_first_of(".:") != std::string::npos) return false;
    const std::string code = NormalizeCode(typed);
    std::vector<std::string> parts(1);
    for (char c : code) {
        if (c == '-') parts.emplace_back();
        else parts.back() += c;
    }
    if (parts.size() != 3) return false;
    for (size_t i = 0; i < 3; i++) {
        if (parts[i].empty()) return false;
        for (char c : parts[i])
            if (i < 2 ? !(c >= 'a' && c <= 'z') : !(c >= '0' && c <= '9')) return false;
    }
    return true;
}

} // namespace relay
