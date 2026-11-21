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
    out.U8(b.axis);
}

bool ReadBlock(ByteReader& in, Block& b) {
    uint8_t type;
    if (!in.U8(type) || type >= static_cast<uint8_t>(BlockType::COUNT)) return false;
    b.type = static_cast<BlockType>(type);
    for (int e = 0; e < 4; e++)
        if (!in.U8(b.edgeData.edges[e])) return false;
    if (!in.U8(b.water) || !in.U8(b.shown) || !in.U8(b.axis)) return false;
    return b.water <= Block::kMaxWaterVolume && b.shown <= Block::kMaxWaterVolume && b.axis <= Block::kAlongZ;
}

void EncodeBlockPlanes(ByteWriter& out, const ChunkBlocks& blocks) {
    EncodePlane(out, [&](int i) { return static_cast<uint8_t>(blocks[i].type); });
    for (int e = 0; e < 4; e++) EncodePlane(out, [&](int i) { return blocks[i].edgeData.edges[e]; });
    EncodePlane(out, [&](int i) { return blocks[i].water; });
    EncodePlane(out, [&](int i) { return blocks[i].shown; });
    EncodePlane(out, [&](int i) { return blocks[i].axis; });
}

void EncodeChunkPlanes(ByteWriter& out, const ChunkBlocks& blocks, const ChunkLight& light) {
    EncodeBlockPlanes(out, blocks);
    EncodeLightPlane(out, light);
}

bool DecodeChunkPlanes(ByteReader& in, ChunkBlocks& blocks, ChunkLight& light) {
    return DecodeBlockPlanes(in, blocks) && DecodeLightPlane(in, light);
}

bool DecodeBlockPlanes(ByteReader& in, ChunkBlocks& blocks, bool withAxis) {
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
    if (!withAxis) {
        for (auto& b : blocks) b.axis = Block::kUpright;
    }
    else if (!DecodePlane(in, [&](int i, uint8_t v) {
                 if (v > Block::kAlongZ) valid = false;
                 blocks[i].axis = v;
             }))
        return false;
    return valid;
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
    w.U8(m.viewDistance);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const Welcome& m) {
    ByteWriter w;
    Begin(w, MsgType::Welcome);
    w.U16(m.playerId);
    w.Vec3(m.spawn);
    w.I32(m.renderDistance);
    w.I32(m.verticalRenderDistance);
    w.U8(m.flags);
    w.F32(m.yaw);
    w.F32(m.pitch);
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
    w.U8(static_cast<uint8_t>(m.tool));
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const Attack& m) {
    ByteWriter w;
    Begin(w, MsgType::Attack);
    w.U16(m.creature);
    w.U8(static_cast<uint8_t>(m.tool));
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const AttackPlayer& m) {
    ByteWriter w;
    Begin(w, MsgType::AttackPlayer);
    w.U16(m.player);
    w.U8(static_cast<uint8_t>(m.tool));
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const CreatureSnapshot& m) {
    ByteWriter w;
    Begin(w, MsgType::CreatureSnapshot);
    w.U32(m.tick);
    const size_t n = std::min(m.creatures.size(), kMaxCreaturesSent);
    w.VarU32(static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; i++) {
        const auto& c = m.creatures[i];
        w.U16(c.id);
        w.U8(static_cast<uint8_t>(c.type));
        w.Vec3(c.feet);
        w.F32(c.yaw);
        w.U8(c.flags);
    }
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const MineStart& m) {
    ByteWriter w;
    Begin(w, MsgType::MineStart);
    w.IVec3(m.pos);
    w.U8(static_cast<uint8_t>(m.tool));
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const EditPlace& m) {
    ByteWriter w;
    Begin(w, MsgType::EditPlace);
    w.IVec3(m.pos);
    w.U8(static_cast<uint8_t>(m.type));
    w.U8(m.height);
    w.U8(m.faceAxis);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const EditChisel& m) {
    ByteWriter w;
    Begin(w, MsgType::EditChisel);
    w.IVec3(m.pos);
    w.U8(m.edge);
    w.U8(m.raise);
    return std::move(w.data);
}

bool Decode(ByteReader& in, EditChisel& m) {
    return in.IVec3(m.pos) && in.U8(m.edge) && in.U8(m.raise) && m.edge < 4 && m.raise <= 1 && in.AtEnd();
}

std::vector<uint8_t> Encode(const SkipTime&) {
    ByteWriter w;
    Begin(w, MsgType::SkipTime);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const Abilities& m) {
    ByteWriter w;
    Begin(w, MsgType::Abilities);
    w.U8(m.flags);
    return std::move(w.data);
}

bool Decode(ByteReader& in, Abilities& m) {
    return in.U8(m.flags) && in.AtEnd();
}

std::vector<uint8_t> Encode(const GameModeIs& m) {
    ByteWriter w;
    Begin(w, MsgType::GameModeIs);
    w.U8(static_cast<uint8_t>(m.mode));
    return std::move(w.data);
}

bool Decode(ByteReader& in, GameModeIs& m) {
    uint8_t mode;
    if (!in.U8(mode) || mode > static_cast<uint8_t>(GameMode::Creative)) return false;
    m.mode = static_cast<GameMode>(mode);
    return in.AtEnd();
}

std::vector<uint8_t> Encode(const Resources& m) {
    ByteWriter w;
    Begin(w, MsgType::Resources);
    w.VarU32(static_cast<uint32_t>(m.held.size()));
    for (const auto& [type, units] : m.held) {
        w.U8(static_cast<uint8_t>(type));
        w.VarU32(units);
    }
    w.U8(m.tools);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const Craft& m) {
    ByteWriter w;
    Begin(w, MsgType::Craft);
    w.U8(m.recipe);
    w.U8(m.count);
    return std::move(w.data);
}

bool Decode(ByteReader& in, Resources& m) {
    uint32_t n;
    if (!in.VarU32(n) || n > static_cast<uint32_t>(BlockType::COUNT)) return false;
    m.held.resize(n);
    for (auto& [type, units] : m.held) {
        uint8_t t;
        if (!in.U8(t) || t >= static_cast<uint8_t>(BlockType::COUNT) || !in.VarU32(units)) return false;
        type = static_cast<BlockType>(t);
    }
    return in.U8(m.tools) && in.AtEnd();
}

bool Decode(ByteReader& in, Craft& m) {
    return in.U8(m.recipe) && in.U8(m.count) && m.count >= 1 && m.count <= 64 && in.AtEnd();
}

std::vector<uint8_t> Encode(const Health& m) {
    ByteWriter w;
    Begin(w, MsgType::Health);
    w.U8(m.health);
    w.U8(m.max);
    w.U8(m.hurt);
    w.U8(m.air);
    w.U8(m.airMax);
    w.U8(m.airCause);
    w.U8(m.food);
    w.U8(m.foodMax);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const Eat& m) {
    ByteWriter w;
    Begin(w, MsgType::Eat);
    w.U8(static_cast<uint8_t>(m.food));
    return std::move(w.data);
}

bool Decode(ByteReader& in, Eat& m) {
    uint8_t type;
    if (!in.U8(type) || type >= static_cast<uint8_t>(BlockType::COUNT)) return false;
    m.food = static_cast<BlockType>(type);
    return in.AtEnd();
}

bool Decode(ByteReader& in, Health& m) {
    return in.U8(m.health) && in.U8(m.max) && in.U8(m.hurt) && in.U8(m.air) && in.U8(m.airMax) && in.U8(m.airCause) && in.U8(m.food) &&
           in.U8(m.foodMax) && m.max > 0 && m.health <= m.max && m.airMax > 0 && m.air <= m.airMax && m.airCause <= 3 && m.foodMax > 0 &&
           m.food <= m.foodMax && in.AtEnd();
}

std::vector<uint8_t> Encode(const ChatSend& m) {
    ByteWriter w;
    Begin(w, MsgType::ChatSend);
    w.Str(m.text.substr(0, kMaxChatLength));
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const ChatMessage& m) {
    ByteWriter w;
    Begin(w, MsgType::ChatMessage);
    w.U8(static_cast<uint8_t>(m.kind));
    w.Str(m.text.substr(0, 2 * kMaxChatLength)); // room for "<name> " in front
    return std::move(w.data);
}

bool Decode(ByteReader& in, ChatSend& m) {
    return in.Str(m.text, kMaxChatLength) && in.AtEnd();
}

bool Decode(ByteReader& in, ChatMessage& m) {
    uint8_t kind;
    if (!in.U8(kind) || kind > static_cast<uint8_t>(ChatMessage::Kind::Private)) return false;
    m.kind = static_cast<ChatMessage::Kind>(kind);
    return in.Str(m.text, 2 * kMaxChatLength);
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
    w.F32(m.timeOfDay);
    w.U8(m.rain);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const Correction& m) {
    ByteWriter w;
    Begin(w, MsgType::Correction);
    w.U16(m.correctionId);
    w.Vec3(m.feet);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const Knockback& m) {
    ByteWriter w;
    Begin(w, MsgType::Knockback);
    w.Vec3(m.velocity);
    return std::move(w.data);
}

// --- Decoding ---------------------------------------------------------------------

namespace {
bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
} // namespace

bool Decode(ByteReader& in, Hello& m) {
    // (The view distance is read only if it's there, so an older client still gets
    // told its version is wrong instead of just being dropped.)
    if (!in.U32(m.version) || !in.Str(m.name, kMaxNameLength)) return false;
    m.viewDistance = 0;
    return in.AtEnd() || in.U8(m.viewDistance);
}

bool Decode(ByteReader& in, Welcome& m) {
    return in.U16(m.playerId) && in.Vec3(m.spawn) && in.I32(m.renderDistance) && in.I32(m.verticalRenderDistance) &&
           in.U8(m.flags) && in.F32(m.yaw) && in.F32(m.pitch) && Finite(m.spawn) && std::isfinite(m.yaw) && std::isfinite(m.pitch) &&
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

namespace {
bool ReadTool(ByteReader& in, Tool& tool) {
    uint8_t t;
    if (!in.U8(t) || t >= static_cast<uint8_t>(Tool::Count)) return false;
    tool = static_cast<Tool>(t);
    return true;
}
} // namespace

bool Decode(ByteReader& in, EditBreak& m) {
    return in.IVec3(m.pos) && ReadTool(in, m.tool);
}

bool Decode(ByteReader& in, MineStart& m) {
    return in.IVec3(m.pos) && ReadTool(in, m.tool);
}

bool Decode(ByteReader& in, Attack& m) {
    return in.U16(m.creature) && ReadTool(in, m.tool) && in.AtEnd();
}

bool Decode(ByteReader& in, AttackPlayer& m) {
    return in.U16(m.player) && ReadTool(in, m.tool) && in.AtEnd();
}

bool Decode(ByteReader& in, CreatureSnapshot& m) {
    uint32_t n;
    if (!in.U32(m.tick) || !in.VarU32(n) || n > kMaxCreaturesSent) return false;
    m.creatures.resize(n);
    for (auto& c : m.creatures) {
        uint8_t type;
        if (!in.U16(c.id) || !in.U8(type) || type >= static_cast<uint8_t>(CreatureType::Count) || !in.Vec3(c.feet) || !in.F32(c.yaw) ||
            !in.U8(c.flags))
            return false;
        c.type = static_cast<CreatureType>(type);
        if (!std::isfinite(c.feet.x) || !std::isfinite(c.feet.y) || !std::isfinite(c.feet.z) || !std::isfinite(c.yaw)) return false;
    }
    return in.AtEnd();
}

bool Decode(ByteReader& in, EditPlace& m) {
    uint8_t type;
    if (!in.IVec3(m.pos) || !in.U8(type) || type >= static_cast<uint8_t>(BlockType::COUNT)) return false;
    m.type = static_cast<BlockType>(type);
    return in.U8(m.height) && m.height >= 1 && m.height <= 8 && in.U8(m.faceAxis) && m.faceAxis <= 2;
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
    return in.F32(m.timeOfDay) && in.U8(m.rain) && std::isfinite(m.timeOfDay) && m.timeOfDay >= 0.0f && m.timeOfDay < 1.0f;
}

bool Decode(ByteReader& in, Knockback& m) {
    return in.Vec3(m.velocity) && Finite(m.velocity) && glm::length(m.velocity) <= kMaxKnockback && in.AtEnd();
}

bool Decode(ByteReader& in, Correction& m) {
    return in.U16(m.correctionId) && in.Vec3(m.feet) && Finite(m.feet);
}

} // namespace net
