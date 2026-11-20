#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "world/Block.h"
#include "world/Chunk.h"

// The client/server message protocol. Every packet is one message: a MsgType byte
// followed by its fields, little endian. Encode* builds a packet; Decode* parses the
// body (after the type byte) and returns false on malformed input, which the receiver
// treats as a protocol error.
//
// Channels:
//  - kChannelReliable (reliable, ordered): handshake, chunks, block deltas, light,
//    edits, corrections. Ordering matters: a block delta always follows the chunk data
//    it applies to.
//  - kChannelUnreliable (unreliable, sequenced): player states and snapshots, which
//    are superseded by the next one anyway.
namespace net {

constexpr uint32_t kProtocolVersion = 1;
constexpr uint16_t kDefaultPort = 27450;
constexpr uint8_t kChannelReliable = 0;
constexpr uint8_t kChannelUnreliable = 1;
constexpr uint8_t kChannelCount = 2;

enum class MsgType : uint8_t {
    // Client -> server
    Hello = 1,     // first message: protocol version and name
    PlayerState,   // own position, look and movement flags (~20 Hz)
    EditBreak,     // break the block at a position
    EditPlace,     // place a block type at a position
    EditCarve,     // laser: clear spheres around a list of centres

    // Server -> client
    Welcome = 64,  // accepted: your id, spawn point, view distance
    Kick,          // refused or dropped, with a reason
    ChunkData,     // a whole chunk (blocks + light), RLE compressed
    ChunkUnload,   // forget a chunk (out of range)
    LightData,     // new light for a chunk you have
    BlockDeltas,   // changed cells in one chunk
    PlayerSnapshot,// every connected player's state
    Correction,    // your position was rejected: move here
};

// ---------------------------------------------------------------------------------
// Byte serialization

class ByteWriter {
public:
    void U8(uint8_t v) { data.push_back(v); }
    void U16(uint16_t v) { U8(static_cast<uint8_t>(v)); U8(static_cast<uint8_t>(v >> 8)); }
    void U32(uint32_t v) { U16(static_cast<uint16_t>(v)); U16(static_cast<uint16_t>(v >> 16)); }
    void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }
    void F32(float v) { uint32_t u; std::memcpy(&u, &v, 4); U32(u); }
    void VarU32(uint32_t v) {
        while (v >= 0x80) { U8(static_cast<uint8_t>(v | 0x80)); v >>= 7; }
        U8(static_cast<uint8_t>(v));
    }
    void Str(const std::string& s) { VarU32(static_cast<uint32_t>(s.size())); data.insert(data.end(), s.begin(), s.end()); }
    void Vec3(const glm::vec3& v) { F32(v.x); F32(v.y); F32(v.z); }
    void IVec3(const glm::ivec3& v) { I32(v.x); I32(v.y); I32(v.z); }
    void Bytes(const uint8_t* p, size_t n) { data.insert(data.end(), p, p + n); }

    std::vector<uint8_t> data;
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : p(data), end(data + size) {}

    bool U8(uint8_t& v) { if (p >= end) return false; v = *p++; return true; }
    bool U16(uint16_t& v) { uint8_t a, b; if (!U8(a) || !U8(b)) return false; v = static_cast<uint16_t>(a | (b << 8)); return true; }
    bool U32(uint32_t& v) { uint16_t a, b; if (!U16(a) || !U16(b)) return false; v = a | (static_cast<uint32_t>(b) << 16); return true; }
    bool I32(int32_t& v) { uint32_t u; if (!U32(u)) return false; v = static_cast<int32_t>(u); return true; }
    bool F32(float& v) { uint32_t u; if (!U32(u)) return false; std::memcpy(&v, &u, 4); return true; }
    bool VarU32(uint32_t& v) {
        v = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            uint8_t b;
            if (!U8(b)) return false;
            v |= static_cast<uint32_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
        }
        return false;
    }
    bool Str(std::string& s, size_t maxLength) {
        uint32_t n;
        if (!VarU32(n) || n > maxLength || n > Remaining()) return false;
        s.assign(reinterpret_cast<const char*>(p), n);
        p += n;
        return true;
    }
    bool Vec3(glm::vec3& v) { return F32(v.x) && F32(v.y) && F32(v.z); }
    bool IVec3(glm::ivec3& v) { return I32(v.x) && I32(v.y) && I32(v.z); }

    size_t Remaining() const { return static_cast<size_t>(end - p); }
    bool AtEnd() const { return p == end; }

private:
    const uint8_t* p;
    const uint8_t* end;
};

// ---------------------------------------------------------------------------------
// Chunk payloads

using ChunkBlocks = std::array<Block, Chunk::VOLUME>;
using ChunkLight = std::array<uint8_t, Chunk::VOLUME>;

// Blocks travel as separate byte planes (type, the four edges, water, shown) and light
// as one more plane, each run-length encoded in layer order (y outermost). Terrain is
// layered, so each plane is mostly a handful of long runs: an all-air or all-stone
// chunk is about 30 bytes, a surface chunk typically one to three kilobytes.
void EncodeChunkPlanes(ByteWriter& out, const ChunkBlocks& blocks, const ChunkLight& light);
bool DecodeChunkPlanes(ByteReader& in, ChunkBlocks& blocks, ChunkLight& light);
void EncodeLightPlane(ByteWriter& out, const ChunkLight& light);
bool DecodeLightPlane(ByteReader& in, ChunkLight& light);

void WriteBlock(ByteWriter& out, const Block& block);
// Rejects unknown block types and impossible water amounts.
bool ReadBlock(ByteReader& in, Block& block);

// ---------------------------------------------------------------------------------
// Messages

struct Hello {
    uint32_t version = kProtocolVersion;
    std::string name;
};

struct Welcome {
    uint16_t playerId = 0;
    glm::vec3 spawn{0.0f}; // feet position; the client drops onto the terrain below it
    int32_t renderDistance = 0;
    int32_t verticalRenderDistance = 0;
};

struct Kick {
    std::string reason;
};

// Player movement state flags.
enum PlayerFlags : uint8_t {
    kFlagFlying = 1 << 0,
    kFlagOnGround = 1 << 1,
    kFlagSwimming = 1 << 2,
};

struct PlayerState {
    uint32_t sequence = 0;
    // The last Correction applied. States sent before it are ignored by the server.
    uint16_t correctionId = 0;
    glm::vec3 feet{0.0f};
    glm::vec3 velocity{0.0f};
    float yaw = 0.0f, pitch = 0.0f; // degrees
    uint8_t flags = 0;
};

struct EditBreak {
    glm::ivec3 pos{0};
};

struct EditPlace {
    glm::ivec3 pos{0};
    BlockType type = BlockType::AIR;
};

struct EditCarve {
    static constexpr int kMaxRadius = 4;
    static constexpr size_t kMaxCenters = 160;
    uint8_t radius = 0;
    std::vector<glm::ivec3> centers;
};

struct ChunkUnload {
    glm::ivec3 coords{0};
};

struct BlockDelta {
    uint16_t index; // chunk-local, x + 16 * (y + 16 * z)
    Block block;
};

struct BlockDeltas {
    glm::ivec3 coords{0};
    std::vector<BlockDelta> cells;
};

struct PlayerInfo {
    uint16_t id = 0;
    glm::vec3 feet{0.0f};
    float yaw = 0.0f, pitch = 0.0f;
    uint8_t flags = 0;
};

struct PlayerSnapshot {
    uint32_t tick = 0;
    std::vector<PlayerInfo> players;
};

struct Correction {
    uint16_t correctionId = 0;
    glm::vec3 feet{0.0f};
};

std::vector<uint8_t> Encode(const Hello& m);
std::vector<uint8_t> Encode(const Welcome& m);
std::vector<uint8_t> Encode(const Kick& m);
std::vector<uint8_t> Encode(const PlayerState& m);
std::vector<uint8_t> Encode(const EditBreak& m);
std::vector<uint8_t> Encode(const EditPlace& m);
std::vector<uint8_t> Encode(const EditCarve& m);
std::vector<uint8_t> EncodeChunkData(const glm::ivec3& coords, const ChunkBlocks& blocks, const ChunkLight& light);
std::vector<uint8_t> Encode(const ChunkUnload& m);
std::vector<uint8_t> EncodeLightData(const glm::ivec3& coords, const ChunkLight& light);
std::vector<uint8_t> Encode(const BlockDeltas& m);
std::vector<uint8_t> Encode(const PlayerSnapshot& m);
std::vector<uint8_t> Encode(const Correction& m);

bool Decode(ByteReader& in, Hello& m);
bool Decode(ByteReader& in, Welcome& m);
bool Decode(ByteReader& in, Kick& m);
bool Decode(ByteReader& in, PlayerState& m);
bool Decode(ByteReader& in, EditBreak& m);
bool Decode(ByteReader& in, EditPlace& m);
bool Decode(ByteReader& in, EditCarve& m);
bool DecodeChunkData(ByteReader& in, glm::ivec3& coords, ChunkBlocks& blocks, ChunkLight& light);
bool Decode(ByteReader& in, ChunkUnload& m);
bool DecodeLightData(ByteReader& in, glm::ivec3& coords, ChunkLight& light);
bool Decode(ByteReader& in, BlockDeltas& m);
bool Decode(ByteReader& in, PlayerSnapshot& m);
bool Decode(ByteReader& in, Correction& m);

} // namespace net
