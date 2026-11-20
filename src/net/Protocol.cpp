#include "net/Protocol.h"

#include <algorithm>
#include <cmath>

namespace net {

namespace {

constexpr int S = Chunk::CHUNK_SIZE;
constexpr size_t kMaxNameLength = 32;
constexpr size_t kMaxReasonLength = 256;
constexpr size_t kMaxPlayers = 256;

// Layer order: y outermost, then z, then x (the chunk's own index is x + S * (y + S * z)).
template <typename Fn>
void ForEachInLayerOrder(Fn fn) {
    for (int y = 0; y < S; y++)
        for (int z = 0; z < S; z++)
            for (int x = 0; x < S; x++) fn(x + S * (y + S * z));
}

// One plane: runs of (length - 1 as varint, value).
template <typename Get>
void EncodePlane(ByteWriter& out, Get get) {
    int runLength = 0;
    uint8_t runValue = 0;
    ForEachInLayerOrder([&](int i) {
        const uint8_t v = get(i);
        if (runLength > 0 && v == runValue) {
            runLength++;
            return;
        }
        if (runLength > 0) {
            out.VarU32(static_cast<uint32_t>(runLength - 1));
            out.U8(runValue);
        }
        runValue = v;
        runLength = 1;
    });
    out.VarU32(static_cast<uint32_t>(runLength - 1));
    out.U8(runValue);
}

template <typename Set>
bool DecodePlane(ByteReader& in, Set set) {
    static const auto order = [] {
        std::array<uint16_t, Chunk::VOLUME> o{};
        int n = 0;
        ForEachInLayerOrder([&](int i) { o[n++] = static_cast<uint16_t>(i); });
        return o;
    }();
    int filled = 0;
    while (filled < Chunk::VOLUME) {
        uint32_t lengthMinusOne;
        uint8_t value;
        if (!in.VarU32(lengthMinusOne) || !in.U8(value)) return false;
        if (lengthMinusOne >= static_cast<uint32_t>(Chunk::VOLUME - filled)) return false;
        for (uint32_t k = 0; k <= lengthMinusOne; k++) set(order[filled++], value);
    }
    return true;
}

void Begin(ByteWriter& w, MsgType type) {
    w.U8(static_cast<uint8_t>(type));
}

} // namespace

void WriteBlock(ByteWriter& out, const Block& b) {
    out.U8(static_cast<uint8_t>(b.type));
    for (int e = 0; e < 4; e++) out.U8(b.edgeData.edges[e]);
    out.U8(b.water);
    out.U8(b.shown);
}

bool ReadBlock(ByteReader& in, Block& b) {
    uint8_t type;
    if (!in.U8(type) || type >= static_cast<uint8_t>(BlockType::COUNT)) return false;
    b.type = static_cast<BlockType>(type);
    for (int e = 0; e < 4; e++)
        if (!in.U8(b.edgeData.edges[e])) return false;
    if (!in.U8(b.water) || !in.U8(b.shown)) return false;
    return b.water <= Block::kMaxWaterVolume && b.shown <= Block::kMaxWaterVolume;
}

void EncodeChunkPlanes(ByteWriter& out, const ChunkBlocks& blocks, const ChunkLight& light) {
    EncodePlane(out, [&](int i) { return static_cast<uint8_t>(blocks[i].type); });
    for (int e = 0; e < 4; e++) EncodePlane(out, [&](int i) { return blocks[i].edgeData.edges[e]; });
    EncodePlane(out, [&](int i) { return blocks[i].water; });
    EncodePlane(out, [&](int i) { return blocks[i].shown; });
    EncodeLightPlane(out, light);
}

bool DecodeChunkPlanes(ByteReader& in, ChunkBlocks& blocks, ChunkLight& light) {
    bool valid = true;
    if (!DecodePlane(in, [&](int i, uint8_t v) {
            if (v >= static_cast<uint8_t>(BlockType::COUNT)) valid = false;
            blocks[i].type = static_cast<BlockType>(v);
        }))
        return false;
    for (int e = 0; e < 4; e++)
        if (!DecodePlane(in, [&](int i, uint8_t v) { blocks[i].edgeData.edges[e] = v; })) return false;
    if (!DecodePlane(in, [&](int i, uint8_t v) {
            if (v > Block::kMaxWaterVolume) valid = false;
            blocks[i].water = v;
        }))
        return false;
    if (!DecodePlane(in, [&](int i, uint8_t v) {
            if (v > Block::kMaxWaterVolume) valid = false;
            blocks[i].shown = v;
        }))
        return false;
    return valid && DecodeLightPlane(in, light);
}

void EncodeLightPlane(ByteWriter& out, const ChunkLight& light) {
    EncodePlane(out, [&](int i) { return light[i]; });
}

bool DecodeLightPlane(ByteReader& in, ChunkLight& light) {
    return DecodePlane(in, [&](int i, uint8_t v) { light[i] = v; });
}

// --- Encoding ---------------------------------------------------------------------

std::vector<uint8_t> Encode(const Hello& m) {
    ByteWriter w;
    Begin(w, MsgType::Hello);
    w.U32(m.version);
    w.Str(m.name.substr(0, kMaxNameLength));
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const Welcome& m) {
    ByteWriter w;
    Begin(w, MsgType::Welcome);
    w.U16(m.playerId);
    w.Vec3(m.spawn);
    w.I32(m.renderDistance);
    w.I32(m.verticalRenderDistance);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const Kick& m) {
    ByteWriter w;
    Begin(w, MsgType::Kick);
    w.Str(m.reason.substr(0, kMaxReasonLength));
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const PlayerState& m) {
    ByteWriter w;
    Begin(w, MsgType::PlayerState);
    w.U32(m.sequence);
    w.U16(m.correctionId);
    w.Vec3(m.feet);
    w.Vec3(m.velocity);
    w.F32(m.yaw);
    w.F32(m.pitch);
    w.U8(m.flags);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const EditBreak& m) {
    ByteWriter w;
    Begin(w, MsgType::EditBreak);
    w.IVec3(m.pos);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const EditPlace& m) {
    ByteWriter w;
    Begin(w, MsgType::EditPlace);
    w.IVec3(m.pos);
    w.U8(static_cast<uint8_t>(m.type));
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const EditCarve& m) {
    ByteWriter w;
    Begin(w, MsgType::EditCarve);
    w.U8(m.radius);
    const size_t n = std::min(m.centers.size(), EditCarve::kMaxCenters);
    w.VarU32(static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; i++) w.IVec3(m.centers[i]);
    return std::move(w.data);
}

std::vector<uint8_t> EncodeChunkData(const glm::ivec3& coords, const ChunkBlocks& blocks, const ChunkLight& light) {
    ByteWriter w;
    w.data.reserve(256);
    Begin(w, MsgType::ChunkData);
    w.IVec3(coords);
    EncodeChunkPlanes(w, blocks, light);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const ChunkUnload& m) {
    ByteWriter w;
    Begin(w, MsgType::ChunkUnload);
    w.IVec3(m.coords);
    return std::move(w.data);
}

std::vector<uint8_t> EncodeLightData(const glm::ivec3& coords, const ChunkLight& light) {
    ByteWriter w;
    Begin(w, MsgType::LightData);
    w.IVec3(coords);
    EncodeLightPlane(w, light);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const BlockDeltas& m) {
    ByteWriter w;
    w.data.reserve(16 + m.cells.size() * 9);
    Begin(w, MsgType::BlockDeltas);
    w.IVec3(m.coords);
    w.VarU32(static_cast<uint32_t>(m.cells.size()));
    for (const auto& c : m.cells) {
        w.U16(c.index);
        WriteBlock(w, c.block);
    }
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const PlayerSnapshot& m) {
    ByteWriter w;
    Begin(w, MsgType::PlayerSnapshot);
    w.U32(m.tick);
    w.VarU32(static_cast<uint32_t>(m.players.size()));
    for (const auto& p : m.players) {
        w.U16(p.id);
        w.Vec3(p.feet);
        w.F32(p.yaw);
        w.F32(p.pitch);
        w.U8(p.flags);
    }
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const Correction& m) {
    ByteWriter w;
    Begin(w, MsgType::Correction);
    w.U16(m.correctionId);
    w.Vec3(m.feet);
    return std::move(w.data);
}

// --- Decoding ---------------------------------------------------------------------

namespace {
bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
} // namespace

bool Decode(ByteReader& in, Hello& m) {
    return in.U32(m.version) && in.Str(m.name, kMaxNameLength);
}

bool Decode(ByteReader& in, Welcome& m) {
    return in.U16(m.playerId) && in.Vec3(m.spawn) && in.I32(m.renderDistance) && in.I32(m.verticalRenderDistance) &&
           m.renderDistance > 0 && m.renderDistance <= 64 && m.verticalRenderDistance > 0 && m.verticalRenderDistance <= 64;
}

bool Decode(ByteReader& in, Kick& m) {
    return in.Str(m.reason, kMaxReasonLength);
}

bool Decode(ByteReader& in, PlayerState& m) {
    return in.U32(m.sequence) && in.U16(m.correctionId) && in.Vec3(m.feet) && in.Vec3(m.velocity) && in.F32(m.yaw) &&
           in.F32(m.pitch) && in.U8(m.flags) && Finite(m.feet) && Finite(m.velocity) && std::isfinite(m.yaw) &&
           std::isfinite(m.pitch);
}

bool Decode(ByteReader& in, EditBreak& m) {
    return in.IVec3(m.pos);
}

bool Decode(ByteReader& in, EditPlace& m) {
    uint8_t type;
    if (!in.IVec3(m.pos) || !in.U8(type) || type >= static_cast<uint8_t>(BlockType::COUNT)) return false;
    m.type = static_cast<BlockType>(type);
    return true;
}

bool Decode(ByteReader& in, EditCarve& m) {
    uint32_t n;
    if (!in.U8(m.radius) || m.radius > EditCarve::kMaxRadius || !in.VarU32(n) || n > EditCarve::kMaxCenters) return false;
    m.centers.resize(n);
    for (auto& c : m.centers)
        if (!in.IVec3(c)) return false;
    return true;
}

bool DecodeChunkData(ByteReader& in, glm::ivec3& coords, ChunkBlocks& blocks, ChunkLight& light) {
    return in.IVec3(coords) && DecodeChunkPlanes(in, blocks, light);
}

bool Decode(ByteReader& in, ChunkUnload& m) {
    return in.IVec3(m.coords);
}

bool DecodeLightData(ByteReader& in, glm::ivec3& coords, ChunkLight& light) {
    return in.IVec3(coords) && DecodeLightPlane(in, light);
}

bool Decode(ByteReader& in, BlockDeltas& m) {
    uint32_t n;
    if (!in.IVec3(m.coords) || !in.VarU32(n) || n > static_cast<uint32_t>(Chunk::VOLUME)) return false;
    m.cells.resize(n);
    for (auto& c : m.cells)
        if (!in.U16(c.index) || c.index >= Chunk::VOLUME || !ReadBlock(in, c.block)) return false;
    return true;
}

bool Decode(ByteReader& in, PlayerSnapshot& m) {
    uint32_t n;
    if (!in.U32(m.tick) || !in.VarU32(n) || n > kMaxPlayers) return false;
    m.players.resize(n);
    for (auto& p : m.players)
        if (!in.U16(p.id) || !in.Vec3(p.feet) || !in.F32(p.yaw) || !in.F32(p.pitch) || !in.U8(p.flags)) return false;
    return true;
}

bool Decode(ByteReader& in, Correction& m) {
    return in.U16(m.correctionId) && in.Vec3(m.feet) && Finite(m.feet);
}

} // namespace net
