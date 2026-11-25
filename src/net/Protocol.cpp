#include "net/Protocol.h"
#include "world/Legacy.h"
#include "world/Signals.h"

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
    // (The axis byte means different things to different blocks: any value is fine.)
    return b.water <= Block::kMaxWaterVolume && b.shown <= Block::kMaxWaterVolume;
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

bool DecodeBlockPlanes(ByteReader& in, ChunkBlocks& blocks, bool withAxis, bool v16) {
    bool valid = true;
    if (!DecodePlane(in, [&](int i, uint8_t v) {
            const BlockType t = v16 ? Legacy::BlockFromV16(v) : static_cast<BlockType>(v);
            if (t >= BlockType::COUNT) valid = false;
            blocks[i].type = t;
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
    else if (!DecodePlane(in, [&](int i, uint8_t v) { blocks[i].axis = v; }))
        return false;
    if (v16)
        for (auto& b : blocks)
            if (b.type == BlockType::FILTER_PIPE) b.axis = Legacy::FilterAxisFromV16(b.axis); // (it held an old block number)
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
    w.U8(static_cast<uint8_t>(m.heldTool));
    w.U16(m.heldItem);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const PlayerName& m) {
    ByteWriter w;
    Begin(w, MsgType::PlayerName);
    w.U16(m.id);
    w.Str(m.name);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const Lightning& m) {
    ByteWriter w;
    Begin(w, MsgType::Lightning);
    w.IVec3(m.pos);
    return std::move(w.data);
}

namespace {
bool FiniteVec(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
} // namespace

std::vector<uint8_t> Encode(const SignText& m, bool fromPlayer) {
    ByteWriter w;
    Begin(w, fromPlayer ? MsgType::WriteSign : MsgType::SignText);
    w.IVec3(m.pos);
    w.Str(m.text.substr(0, kMaxSignLength));
    return std::move(w.data);
}

bool Decode(ByteReader& in, SignText& m) {
    return in.IVec3(m.pos) && in.Str(m.text, kMaxSignLength) && in.AtEnd();
}

std::vector<uint8_t> Encode(const Cast& m) {
    ByteWriter w;
    Begin(w, MsgType::Cast);
    w.IVec3(m.pos);
    w.U8(m.cast);
    return std::move(w.data);
}

bool Decode(ByteReader& in, Cast& m) {
    return in.IVec3(m.pos) && in.U8(m.cast) && m.cast <= 1 && in.AtEnd();
}

std::vector<uint8_t> Encode(const Bobber& m) {
    ByteWriter w;
    Begin(w, MsgType::Bobber);
    w.IVec3(m.pos);
    w.U8(m.out);
    w.U8(m.bite);
    return std::move(w.data);
}

bool Decode(ByteReader& in, Bobber& m) {
    return in.IVec3(m.pos) && in.U8(m.out) && in.U8(m.bite) && m.out <= 1 && m.bite <= 1 && in.AtEnd();
}

std::vector<uint8_t> Encode(const Shoot& m) {
    ByteWriter w;
    Begin(w, MsgType::Shoot);
    w.Vec3(m.direction);
    return std::move(w.data);
}

bool Decode(ByteReader& in, Shoot& m) {
    return in.Vec3(m.direction) && FiniteVec(m.direction) && glm::length(m.direction) > 0.1f && in.AtEnd();
}

std::vector<uint8_t> Encode(const DropItem& m) {
    ByteWriter w;
    Begin(w, MsgType::DropItem);
    w.U8(m.box);
    w.U8(m.slot);
    w.U8(m.count);
    w.Vec3(m.direction);
    w.U8(m.strength);
    return std::move(w.data);
}

bool Decode(ByteReader& in, DropItem& m) {
    return in.U8(m.box) && m.box <= 2 && in.U8(m.slot) && in.U8(m.count) && in.Vec3(m.direction) && FiniteVec(m.direction) && in.U8(m.strength) && in.AtEnd();
}

std::vector<uint8_t> Encode(const Flatten& m) {
    ByteWriter w;
    Begin(w, MsgType::Flatten);
    w.IVec3(m.pos);
    w.U8(m.radius);
    return std::move(w.data);
}

bool Decode(ByteReader& in, Flatten& m) { return in.IVec3(m.pos) && in.U8(m.radius) && m.radius >= 1 && m.radius <= kMaxFlattenRadius && in.AtEnd(); }

std::vector<uint8_t> Encode(const UseBucket& m) {
    ByteWriter w;
    Begin(w, MsgType::UseBucket);
    w.IVec3(m.pos);
    w.U8(m.pour);
    return std::move(w.data);
}

bool Decode(ByteReader& in, UseBucket& m) { return in.IVec3(m.pos) && in.U8(m.pour) && m.pour <= 1 && in.AtEnd(); }

std::vector<uint8_t> Encode(const ItemsNear& m) {
    ByteWriter w;
    Begin(w, MsgType::ItemsNear);
    const size_t n = std::min(m.items.size(), kMaxItemsSent);
    w.VarU32(static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; i++) {
        w.VarU32(m.items[i].id);
        w.U16(m.items[i].item);
        w.U8(m.items[i].count);
        w.Vec3(m.items[i].position);
        w.Vec3(m.items[i].velocity);
    }
    return std::move(w.data);
}

bool Decode(ByteReader& in, ItemsNear& m) {
    uint32_t n;
    if (!in.VarU32(n) || n > kMaxItemsSent) return false;
    m.items.resize(n);
    for (auto& i : m.items)
        if (!in.VarU32(i.id) || !in.U16(i.item) || !in.U8(i.count) || !in.Vec3(i.position) || !in.Vec3(i.velocity) || !FiniteVec(i.position) ||
            !FiniteVec(i.velocity))
            return false;
    return in.AtEnd();
}

std::vector<uint8_t> Encode(const SteamNear& m) {
    ByteWriter w;
    Begin(w, MsgType::SteamNear);
    w.IVec3(m.base);
    const size_t n = std::min(m.cells.size(), kMaxSteamSent);
    w.VarU32(static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; i++) {
        w.U16(static_cast<uint16_t>(m.cells[i].dx));
        w.U16(static_cast<uint16_t>(m.cells[i].dy));
        w.U16(static_cast<uint16_t>(m.cells[i].dz));
        w.U8(m.cells[i].density);
    }
    return std::move(w.data);
}

bool Decode(ByteReader& in, SteamNear& m) {
    uint32_t n;
    if (!in.IVec3(m.base) || !in.VarU32(n) || n > kMaxSteamSent) return false;
    m.cells.resize(n);
    for (auto& c : m.cells) {
        uint16_t x, y, z;
        if (!in.U16(x) || !in.U16(y) || !in.U16(z) || !in.U8(c.density)) return false;
        c.dx = static_cast<int16_t>(x), c.dy = static_cast<int16_t>(y), c.dz = static_cast<int16_t>(z);
    }
    return in.AtEnd();
}

std::vector<uint8_t> Encode(const Arrows& m) {
    ByteWriter w;
    Begin(w, MsgType::Arrows);
    const size_t n = std::min(m.arrows.size(), kMaxArrowsSent);
    w.VarU32(static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; i++) {
        w.Vec3(m.arrows[i].position);
        w.Vec3(m.arrows[i].velocity);
    }
    return std::move(w.data);
}

bool Decode(ByteReader& in, Arrows& m) {
    uint32_t n;
    if (!in.VarU32(n) || n > kMaxArrowsSent) return false;
    m.arrows.resize(n);
    for (auto& a : m.arrows)
        if (!in.Vec3(a.position) || !in.Vec3(a.velocity) || !FiniteVec(a.position) || !FiniteVec(a.velocity)) return false;
    return in.AtEnd();
}

std::vector<uint8_t> Encode(const Inspect& m) {
    ByteWriter w;
    Begin(w, MsgType::Inspect);
    w.IVec3(m.pos);
    return std::move(w.data);
}

bool Decode(ByteReader& in, Inspect& m) {
    return in.IVec3(m.pos) && in.AtEnd();
}

std::vector<uint8_t> Encode(const SignalInfo& m, bool configure) {
    ByteWriter w;
    Begin(w, configure ? MsgType::ConfigureSignal : MsgType::SignalInfo);
    w.IVec3(m.pos);
    for (const uint8_t v : { m.kind, m.mode, m.a, m.b, m.out, m.on, m.erased }) w.U8(v);
    return std::move(w.data);
}

bool Decode(ByteReader& in, SignalInfo& m) {
    return in.IVec3(m.pos) && in.U8(m.kind) && in.U8(m.mode) && in.U8(m.a) && in.U8(m.b) && in.U8(m.out) && in.U8(m.on) && in.U8(m.erased) &&
           m.kind < static_cast<uint8_t>(Signals::Kind::Count) && m.mode < static_cast<uint8_t>(Signals::Mode::Count) && m.on <= 1 && m.erased <= 1 && in.AtEnd();
}

std::vector<uint8_t> Encode(const Machines& m) {
    ByteWriter w;
    Begin(w, MsgType::Machines);
    const size_t n = std::min(m.machines.size(), kMaxMachinesSent);
    w.VarU32(static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; i++) {
        const auto& x = m.machines[i];
        w.IVec3(x.pos);
        w.U8(x.type);
        w.U8(x.speed);
        w.U8(x.pressure);
        w.U8(x.state);
        w.U8(x.depth);
    }
    return std::move(w.data);
}

bool Decode(ByteReader& in, Machines& m) {
    uint32_t n;
    if (!in.VarU32(n) || n > kMaxMachinesSent) return false;
    m.machines.resize(n);
    for (auto& x : m.machines)
        if (!in.IVec3(x.pos) || !in.U8(x.type) || !in.U8(x.speed) || !in.U8(x.pressure) || !in.U8(x.state) || !in.U8(x.depth) ||
            x.type >= static_cast<uint8_t>(BlockType::COUNT))
            return false;
    return in.AtEnd();
}

std::vector<uint8_t> Encode(const Explosion& m) {
    ByteWriter w;
    Begin(w, MsgType::Explosion);
    w.IVec3(m.pos);
    w.U8(m.radius);
    return std::move(w.data);
}

bool Decode(ByteReader& in, Explosion& m) {
    return in.IVec3(m.pos) && in.U8(m.radius) && m.radius <= 8 && in.AtEnd();
}

bool Decode(ByteReader& in, Lightning& m) {
    return in.IVec3(m.pos) && in.AtEnd();
}

bool Decode(ByteReader& in, PlayerName& m) {
    return in.U16(m.id) && in.Str(m.name, 64) && in.AtEnd();
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

std::vector<uint8_t> Encode(const Interact& m) {
    ByteWriter w;
    Begin(w, MsgType::Interact);
    w.U16(m.creature);
    return std::move(w.data);
}

bool Decode(ByteReader& in, Interact& m) {
    return in.U16(m.creature) && in.AtEnd();
}

std::vector<uint8_t> Encode(const Trade& m) {
    ByteWriter w;
    Begin(w, MsgType::Trade);
    w.U16(m.creature);
    w.U8(m.offer);
    return std::move(w.data);
}

bool Decode(ByteReader& in, Trade& m) {
    return in.U16(m.creature) && in.U8(m.offer) && in.AtEnd();
}

std::vector<uint8_t> Encode(const Sites& m) {
    ByteWriter w;
    Begin(w, MsgType::Sites);
    const size_t n = std::min(m.sites.size(), kMaxSitesSent);
    w.VarU32(static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; i++) {
        w.U8(m.sites[i].kind);
        w.U8(m.sites[i].fallen);
        w.IVec3(m.sites[i].centre);
    }
    w.U8(m.hasBed);
    w.IVec3(m.bed);
    return std::move(w.data);
}

bool Decode(ByteReader& in, Sites& m) {
    uint32_t n;
    if (!in.VarU32(n) || n > kMaxSitesSent) return false;
    m.sites.resize(n);
    for (auto& s : m.sites)
        if (!in.U8(s.kind) || !in.U8(s.fallen) || !in.IVec3(s.centre) || s.kind > 3 || s.fallen > 1) return false;
    return in.U8(m.hasBed) && in.IVec3(m.bed) && m.hasBed <= 1 && in.AtEnd();
}

std::vector<uint8_t> Encode(const TradePrices& m) {
    ByteWriter w;
    Begin(w, MsgType::TradePrices);
    w.U16(m.creature);
    w.VarU32(static_cast<uint32_t>(std::min<size_t>(m.give.size(), 64)));
    for (size_t i = 0; i < m.give.size() && i < 64; i++) w.U16(m.give[i]);
    return std::move(w.data);
}

bool Decode(ByteReader& in, TradePrices& m) {
    uint32_t n;
    if (!in.U16(m.creature) || !in.VarU32(n) || n > 64) return false;
    m.give.resize(n);
    for (auto& g : m.give)
        if (!in.U16(g)) return false;
    return in.AtEnd();
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
        w.U8(c.health);
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
    w.U8(m.dir);
    w.U8(m.step);
    return std::move(w.data);
}

bool Decode(ByteReader& in, EditChisel& m) {
    return in.IVec3(m.pos) && in.U8(m.edge) && in.U8(m.raise) && in.U8(m.dir) && in.U8(m.step) && m.edge < 4 && m.raise <= 1 && m.dir <= 5 &&
           Block::IsChiselStep(m.step) && in.AtEnd();
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

void WriteInventory(ByteWriter& w, const ::Inventory& inv) {
    w.VarU32(static_cast<uint32_t>(inv.slots.size()));
    for (const Stack& st : inv.slots) {
        if (st.Empty()) {
            w.U16(0);
            continue;
        }
        w.U16(st.item);
        w.U8(st.count);
    }
    uint32_t kinds = 0;
    for (uint8_t o : inv.offcuts) kinds += o > 0;
    w.VarU32(kinds);
    for (size_t t = 0; t < inv.offcuts.size(); t++)
        if (inv.offcuts[t] > 0) {
            w.U8(static_cast<uint8_t>(t));
            w.U8(inv.offcuts[t]);
        }
}

bool ReadInventory(ByteReader& in, ::Inventory& inv, size_t maxSlots, bool v16) {
    uint32_t n;
    if (!in.VarU32(n) || n > maxSlots) return false;
    inv.slots.assign(n, Stack{});
    inv.offcuts.fill(0);
    for (Stack& st : inv.slots) {
        uint16_t item;
        if (!in.U16(item)) return false;
        if (item == 0) continue;
        uint8_t count;
        if (v16) item = Legacy::FromV16(item);
        if (!Items::Valid(item) || !in.U8(count) || count == 0 || count > Items::MaxStack(item)) return false;
        if (Items::IsBlock(item)) item = Items::OfBlock(Current(Items::BlockOf(item))); // (old water pipes are fluid pipes now)
        st = { item, count };
    }
    uint32_t kinds;
    if (!in.VarU32(kinds) || kinds > inv.offcuts.size()) return false;
    for (uint32_t k = 0; k < kinds; k++) {
        uint8_t t, units;
        if (!in.U8(t) || !in.U8(units) || units >= Items::kUnits) return false;
        ItemId item = v16 ? Legacy::ItemFromV16Block(t) : Items::OfKind(t);
        if (!Items::IsStackable(item)) return false;
        if (Items::IsBlock(item)) item = Items::OfBlock(Current(Items::BlockOf(item)));
        const size_t kept = Items::KindOf(item);
        inv.offcuts[kept] = static_cast<uint8_t>(std::min<int>(Items::kUnits - 1, inv.offcuts[kept] + units));
    }
    return true;
}

std::vector<uint8_t> Encode(const InventoryIs& m) {
    ByteWriter w;
    Begin(w, MsgType::InventoryIs);
    WriteInventory(w, m.items);
    w.U8(m.armour);
    return std::move(w.data);
}

bool Decode(ByteReader& in, InventoryIs& m) {
    return ReadInventory(in, m.items) && in.U8(m.armour) && in.AtEnd();
}

std::vector<uint8_t> Encode(const InventoryMove& m) {
    ByteWriter w;
    Begin(w, MsgType::InventoryMove);
    w.U8(static_cast<uint8_t>(m.fromBox));
    w.U8(m.fromSlot);
    w.U8(static_cast<uint8_t>(m.toBox));
    w.U8(m.toSlot);
    w.U8(m.count);
    return std::move(w.data);
}

bool Decode(ByteReader& in, InventoryMove& m) {
    uint8_t from, to;
    if (!in.U8(from) || from > 2 || !in.U8(m.fromSlot) || !in.U8(to) || to > 2 || !in.U8(m.toSlot) || !in.U8(m.count)) return false;
    m.fromBox = static_cast<Box>(from);
    m.toBox = static_cast<Box>(to);
    return in.AtEnd();
}

std::vector<uint8_t> Encode(const CreativeSlot& m) {
    ByteWriter w;
    Begin(w, MsgType::CreativeSlot);
    w.U8(m.slot);
    w.U16(m.item);
    w.U8(m.count);
    return std::move(w.data);
}

bool Decode(ByteReader& in, CreativeSlot& m) {
    return in.U8(m.slot) && in.U16(m.item) && (m.item == Items::kNone || Items::Valid(m.item)) && in.U8(m.count) && in.AtEnd();
}

std::vector<uint8_t> Encode(const Craft& m) {
    ByteWriter w;
    Begin(w, MsgType::Craft);
    w.U8(m.recipe);
    w.U8(m.count);
    w.U8(static_cast<uint8_t>(m.tool));
    return std::move(w.data);
}

bool Decode(ByteReader& in, Craft& m) {
    uint8_t tool;
    if (!in.U8(m.recipe) || !in.U8(m.count) || m.count < 1 || m.count > 64 || !in.U8(tool) || tool >= static_cast<uint8_t>(Tool::Count)) return false;
    m.tool = static_cast<Tool>(tool);
    return in.AtEnd();
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
    w.U8(m.tank);
    w.U8(m.onTank);
    w.U8(m.burning);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const Eat& m) {
    ByteWriter w;
    Begin(w, MsgType::Eat);
    w.U16(m.food);
    return std::move(w.data);
}

std::vector<uint8_t> Encode(const UseBed& m) {
    ByteWriter w;
    Begin(w, MsgType::UseBed);
    w.IVec3(m.pos);
    return std::move(w.data);
}

bool Decode(ByteReader& in, UseBed& m) {
    return in.IVec3(m.pos) && in.AtEnd();
}

std::vector<uint8_t> Encode(const OpenChest& m) {
    ByteWriter w;
    Begin(w, MsgType::OpenChest);
    w.IVec3(m.pos);
    w.U8(m.open);
    return std::move(w.data);
}

bool Decode(ByteReader& in, OpenChest& m) {
    return in.IVec3(m.pos) && in.U8(m.open) && m.open <= 1 && in.AtEnd();
}

std::vector<uint8_t> Encode(const ChestMove& m) {
    ByteWriter w;
    Begin(w, MsgType::ChestMove);
    w.IVec3(m.pos);
    w.U16(m.item);
    w.VarU32(m.count);
    w.U8(m.intoChest);
    return std::move(w.data);
}

bool Decode(ByteReader& in, ChestMove& m) {
    if (!in.IVec3(m.pos) || !in.U16(m.item) || !Items::Valid(m.item)) return false;
    return in.VarU32(m.count) && in.U8(m.intoChest) && m.intoChest <= 1 && in.AtEnd();
}

std::vector<uint8_t> Encode(const ChestContents& m) {
    ByteWriter w;
    Begin(w, MsgType::ChestContents);
    w.IVec3(m.pos);
    w.U8(m.open);
    WriteInventory(w, m.items);
    return std::move(w.data);
}

bool Decode(ByteReader& in, ChestContents& m) {
    return in.IVec3(m.pos) && in.U8(m.open) && m.open <= 1 && ReadInventory(in, m.items) && in.AtEnd();
}

bool Decode(ByteReader& in, Eat& m) {
    return in.U16(m.food) && Items::Valid(m.food) && in.AtEnd();
}

bool Decode(ByteReader& in, Health& m) {
    return in.U8(m.health) && in.U8(m.max) && in.U8(m.hurt) && in.U8(m.air) && in.U8(m.airMax) && in.U8(m.airCause) && in.U8(m.food) &&
           in.U8(m.foodMax) && in.U8(m.tank) && in.U8(m.onTank) && in.U8(m.burning) && m.onTank <= 1 && m.burning <= 1 && m.max > 0 && m.health <= m.max && m.airMax > 0 && m.air <= m.airMax && m.airCause <= 3 && m.foodMax > 0 &&
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
    if (!in.U8(kind) || kind > static_cast<uint8_t>(ChatMessage::Kind::Inspect)) return false;
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
        w.U8(static_cast<uint8_t>(p.heldTool));
        w.U16(p.heldItem);
        w.U8(p.armour);
    }
    w.F32(m.timeOfDay);
    w.U8(m.rain);
    w.U8(m.storm);
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

namespace {
// What a player holds: a tool and an item (or none), both real.
bool ReadHeld(ByteReader& in, Tool& tool, ItemId& item) {
    uint8_t t;
    if (!in.U8(t) || !in.U16(item) || t >= static_cast<uint8_t>(Tool::Count) || (item != Items::kNone && !Items::Valid(item))) return false;
    tool = static_cast<Tool>(t);
    return true;
}
} // namespace

bool Decode(ByteReader& in, PlayerState& m) {
    return in.U32(m.sequence) && in.U16(m.correctionId) && in.Vec3(m.feet) && in.Vec3(m.velocity) && in.F32(m.yaw) &&
           in.F32(m.pitch) && in.U8(m.flags) && ReadHeld(in, m.heldTool, m.heldItem) && Finite(m.feet) && Finite(m.velocity) &&
           std::isfinite(m.yaw) && std::isfinite(m.pitch);
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
            !in.U8(c.flags) || !in.U8(c.health))
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
    return in.U8(m.height) && m.height >= 1 && m.height <= 8 && in.U8(m.faceAxis) && (m.faceAxis & 7) <= 5 && m.faceAxis < 16; // (8: a door hinged on the right)
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
        if (!in.U16(p.id) || !in.Vec3(p.feet) || !in.F32(p.yaw) || !in.F32(p.pitch) || !in.U8(p.flags) || !ReadHeld(in, p.heldTool, p.heldItem) ||
            !in.U8(p.armour))
            return false;
    return in.F32(m.timeOfDay) && in.U8(m.rain) && in.U8(m.storm) && m.storm <= 1 && std::isfinite(m.timeOfDay) && m.timeOfDay >= 0.0f && m.timeOfDay < 1.0f;
}

bool Decode(ByteReader& in, Knockback& m) {
    return in.Vec3(m.velocity) && Finite(m.velocity) && glm::length(m.velocity) <= kMaxKnockback && in.AtEnd();
}

bool Decode(ByteReader& in, Correction& m) {
    return in.U16(m.correctionId) && in.Vec3(m.feet) && Finite(m.feet);
}

} // namespace net
