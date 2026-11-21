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
#include "world/Creatures.h"
#include "world/Mining.h"
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

constexpr uint32_t kProtocolVersion = 27;
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
    SkipTime,      // jump to the next dawn (at night) or dusk (by day)
    ChatSend,      // a chat line, or a command if it starts with '/'
    EditChisel,    // lower (or raise) one quarter of a block by a quarter of a block
    MineStart,     // started mining a block (survival: breaking it takes time)
    Attack,        // hit a creature
    Craft,         // make something from resources (survival)
    AttackPlayer,  // hit another player (when PvP is on)
    Eat,           // eat a food item (survival)
    OpenChest,     // open the chest at a position (or close the open one)
    ChestMove,     // move units of a type between you and the open chest
    UseBed,        // right-clicked a bed: respawn there (and sleep, at night)

    // Server -> client
    Welcome = 64,  // accepted: your id, spawn point, view distance
    Kick,          // refused or dropped, with a reason
    ChunkData,     // a whole chunk (blocks + light), RLE compressed
    ChunkUnload,   // forget a chunk (out of range)
    LightData,     // new light for a chunk you have
    BlockDeltas,   // changed cells in one chunk
    PlayerSnapshot,// every connected player's state, the time of day and the weather
    Correction,    // your position was rejected: move here
    ChatMessage,   // a line for the chat log: someone's chat, or a server message
    Abilities,     // what you may do: operator (laser), no-clip
    Health,        // your health changed (hurt, healing, or respawned)
    GameModeIs,    // you're in creative or survival
    Resources,     // what you hold (survival): units of each block type
    CreatureSnapshot, // the creatures near you
    PlayerName,    // a player's name, by id (for their name tag)
    ChestContents, // what's in the chest you have open (or that it closed)
    Lightning,     // lightning struck here (in a storm)
    Knockback,     // you were hit: a push to add to your velocity
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
    bool Skip(size_t n) { if (n > Remaining()) return false; p += n; return true; }
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
// Just the block planes (saved chunks store no light; it's recomputed on load).
void EncodeBlockPlanes(ByteWriter& out, const ChunkBlocks& blocks);
// withAxis: false for planes saved before blocks had an axis (region format < 5).
bool DecodeBlockPlanes(ByteReader& in, ChunkBlocks& blocks, bool withAxis = true);
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
    // How far the client wants to see, in chunks (0: as far as the server goes). The
    // server sends no further than this, and no further than its own limit.
    uint8_t viewDistance = 0;
};

struct Welcome {
    uint16_t playerId = 0;
    glm::vec3 spawn{0.0f}; // feet position; the client drops onto the terrain below it
    int32_t renderDistance = 0;
    int32_t verticalRenderDistance = 0;
    // kWelcomeResume: `spawn` is where this player was when they last left (from the
    // saved world): start exactly there, facing yaw/pitch, instead of dropping onto
    // the terrain. kFlagFlying: they were flying.
    uint8_t flags = 0;
    float yaw = -90.0f, pitch = 0.0f;
};
constexpr uint8_t kWelcomeResume = 0x80;
constexpr uint8_t kWelcomeEmptyWorld = 0x40; // the world is a flat empty plain (no biomes)

struct Kick {
    std::string reason;
};

// Player movement state flags.
enum PlayerFlags : uint8_t {
    kFlagFlying = 1 << 0,
    kFlagOnGround = 1 << 1,
    kFlagSwimming = 1 << 2,
    kFlagHurt = 1 << 3, // (from the server, in snapshots) just took damage: flashes red
    kFlagCrouching = 1 << 4, // crouching: a shorter body (Player::kCrouchHeight), drawn ducked
};

struct PlayerState {
    uint32_t sequence = 0;
    // The last Correction applied. States sent before it are ignored by the server.
    uint16_t correctionId = 0;
    glm::vec3 feet{0.0f};
    glm::vec3 velocity{0.0f};
    float yaw = 0.0f, pitch = 0.0f; // degrees
    uint8_t flags = 0;
    // What's in their hand, for others to see: a tool, or else a block or item
    // (BlockType::AIR: nothing).
    Tool heldTool = Tool::None;
    BlockType heldBlock = BlockType::AIR;
};

struct EditBreak {
    glm::ivec3 pos{0};
    Tool tool = Tool::None; // what it was mined with (see world/Mining.h)
};

// Survival: the client started mining this block, with this tool. The server times
// it from here, and only lets the EditBreak through once it's had long enough.
struct MineStart {
    glm::ivec3 pos{0};
    Tool tool = Tool::None;
};

struct EditPlace {
    glm::ivec3 pos{0};
    BlockType type = BlockType::AIR;
    uint8_t height = 8; // eighths: a slab lower than full (blocks that can be shaped)
    uint8_t faceAxis = 1; // axis of the face it was placed against (0 x, 1 y, 2 z): logs lie along it
};

// Game modes. Creative: build with anything, nothing hurts you. Survival: blocks cost
// the resources you've collected by breaking blocks, and traps hurt.
enum class GameMode : uint8_t { Survival = 0, Creative = 1 };
struct GameModeIs {
    GameMode mode = GameMode::Survival;
};
// Units held of each block type (see Block::ResourceUnits), types with none left out.
struct Resources {
    std::vector<std::pair<BlockType, uint32_t>> held;
    uint8_t tools = 0; // tools made (Crafting::ToolBit each)
    uint8_t armour = 0; // armour worn (Armour::Bit each)
};

// Survival: make `count` of a recipe (an index into Crafting::Recipes()).
struct Craft {
    uint8_t recipe = 0;
    uint8_t count = 1;
};

struct EditChisel {
    glm::ivec3 pos{0};
    uint8_t edge = 0;  // which quarter (EdgeData order, 0..3)
    uint8_t raise = 0; // 1: raise it instead of lowering it
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
    Tool heldTool = Tool::None;          // see PlayerState
    BlockType heldBlock = BlockType::AIR;
    uint8_t armour = 0;                  // armour worn (Armour::Bit each)
};

// A player's name, sent to everyone when they join (and to a joining player for each
// player already there), for name tags.
struct PlayerName {
    uint16_t id = 0;
    std::string name;
};

struct PlayerSnapshot {
    uint32_t tick = 0;
    std::vector<PlayerInfo> players;
    float timeOfDay = 0.0f; // 0..1: 0 sunrise, 0.25 noon, 0.5 sunset, 0.75 midnight
    uint8_t rain = 0;       // rain intensity, 0..255 (fades in and out)
    uint8_t storm = 0;      // 1: the rain is a thunderstorm (darker, with lightning)
};

// Lightning struck the ground here (the top of the column).
struct Lightning {
    glm::ivec3 pos{0};
};

struct SkipTime {};

// Creatures (world/Creatures.h): the client hits one; the server says where the ones
// near each player are, every tick.
struct Attack {
    uint16_t creature = 0;
    Tool tool = Tool::None;
};
// Hitting another player (PvP, when the server allows it).
struct AttackPlayer {
    uint16_t player = 0;
    Tool tool = Tool::None;
};
constexpr uint8_t kCreatureHurt = 1 << 0; // flashes red
struct CreatureInfo {
    uint16_t id = 0;
    CreatureType type = CreatureType::Deer;
    glm::vec3 feet{0.0f};
    float yaw = 0.0f;
    uint8_t flags = 0;
};
struct CreatureSnapshot {
    uint32_t tick = 0;
    std::vector<CreatureInfo> creatures;
};
constexpr size_t kMaxCreaturesSent = 128;

// What the server lets a player do. No-clip is switched with /noclip (operators); the
// laser is for operators. The server enforces both.
constexpr uint8_t kAbilityOperator = 1 << 0;
constexpr uint8_t kAbilityNoclip = 1 << 1;
struct Abilities {
    uint8_t flags = 0;
};

// Health in half hearts (20 = ten hearts). `hurt` is set when this change was damage.
struct Health {
    uint8_t health = 20, max = 20;
    uint8_t hurt = 0;
    // Breath (survival): air left, 0..airMax (Breathing::kMaxAir), and what's taking
    // it (Breathing::Cause; 0 none).
    uint8_t air = 20, airMax = 20;
    uint8_t airCause = 0;
    // Hunger (survival, world/Hunger.h): food points, 0..foodMax.
    uint8_t food = 20, foodMax = 20;
};

// Chests: open one (in reach) to see what's in it, move things in and out, close it.
// `open` 0 closes whatever chest is open.
struct OpenChest {
    glm::ivec3 pos{0};
    uint8_t open = 1;
};
struct ChestMove {
    glm::ivec3 pos{0};
    BlockType type = BlockType::AIR;
    uint32_t units = 0;
    uint8_t intoChest = 1; // 1: from you into the chest; 0: out of it
};
// What's in the open chest, by type (none left out), sent on opening and whenever it
// changes; `open` 0: it's closed (broken, out of reach, or you closed it).
struct ChestContents {
    glm::ivec3 pos{0};
    uint8_t open = 1;
    std::vector<std::pair<BlockType, uint32_t>> held;
};

// Right-clicked a bed (in reach).
struct UseBed {
    glm::ivec3 pos{0};
};

// Eat one of a food item you hold.
struct Eat {
    BlockType food = BlockType::AIR;
};

// Chat: what a player typed, and lines for the chat log.
constexpr size_t kMaxChatLength = 256;
struct ChatSend {
    std::string text;
};
struct ChatMessage {
    enum class Kind : uint8_t { Chat = 0, Info = 1, Error = 2, Private = 3 };
    Kind kind = Kind::Chat;
    std::string text;
};

struct Correction {
    uint16_t correctionId = 0;
    glm::vec3 feet{0.0f};
};

// Hit by another player: the client adds this to its velocity (it moves itself).
struct Knockback {
    glm::vec3 velocity{0.0f};
};
constexpr float kMaxKnockback = 20.0f; // blocks a second; anything faster is refused

std::vector<uint8_t> Encode(const Hello& m);
std::vector<uint8_t> Encode(const Welcome& m);
std::vector<uint8_t> Encode(const Kick& m);
std::vector<uint8_t> Encode(const PlayerState& m);
std::vector<uint8_t> Encode(const EditBreak& m);
std::vector<uint8_t> Encode(const MineStart& m);
std::vector<uint8_t> Encode(const Attack& m);
std::vector<uint8_t> Encode(const AttackPlayer& m);
std::vector<uint8_t> Encode(const Eat& m);
std::vector<uint8_t> Encode(const OpenChest& m);
std::vector<uint8_t> Encode(const UseBed& m);
std::vector<uint8_t> Encode(const ChestMove& m);
std::vector<uint8_t> Encode(const ChestContents& m);
std::vector<uint8_t> Encode(const Craft& m);
std::vector<uint8_t> Encode(const CreatureSnapshot& m);
std::vector<uint8_t> Encode(const EditPlace& m);
std::vector<uint8_t> Encode(const EditCarve& m);
std::vector<uint8_t> Encode(const SkipTime& m);
std::vector<uint8_t> Encode(const EditChisel& m);
bool Decode(ByteReader& in, EditChisel& m);
std::vector<uint8_t> Encode(const Abilities& m);
bool Decode(ByteReader& in, Abilities& m);
std::vector<uint8_t> Encode(const Health& m);
bool Decode(ByteReader& in, Health& m);
std::vector<uint8_t> Encode(const GameModeIs& m);
bool Decode(ByteReader& in, GameModeIs& m);
std::vector<uint8_t> Encode(const Resources& m);
bool Decode(ByteReader& in, Resources& m);
std::vector<uint8_t> Encode(const ChatSend& m);
std::vector<uint8_t> Encode(const ChatMessage& m);
bool Decode(ByteReader& in, ChatSend& m);
bool Decode(ByteReader& in, ChatMessage& m);
std::vector<uint8_t> EncodeChunkData(const glm::ivec3& coords, const ChunkBlocks& blocks, const ChunkLight& light);
std::vector<uint8_t> Encode(const ChunkUnload& m);
std::vector<uint8_t> EncodeLightData(const glm::ivec3& coords, const ChunkLight& light);
std::vector<uint8_t> Encode(const BlockDeltas& m);
std::vector<uint8_t> Encode(const PlayerSnapshot& m);
std::vector<uint8_t> Encode(const Correction& m);
std::vector<uint8_t> Encode(const PlayerName& m);
std::vector<uint8_t> Encode(const Lightning& m);
std::vector<uint8_t> Encode(const Knockback& m);

bool Decode(ByteReader& in, Hello& m);
bool Decode(ByteReader& in, Welcome& m);
bool Decode(ByteReader& in, Kick& m);
bool Decode(ByteReader& in, PlayerState& m);
bool Decode(ByteReader& in, EditBreak& m);
bool Decode(ByteReader& in, MineStart& m);
bool Decode(ByteReader& in, Attack& m);
bool Decode(ByteReader& in, AttackPlayer& m);
bool Decode(ByteReader& in, Eat& m);
bool Decode(ByteReader& in, OpenChest& m);
bool Decode(ByteReader& in, UseBed& m);
bool Decode(ByteReader& in, ChestMove& m);
bool Decode(ByteReader& in, ChestContents& m);
bool Decode(ByteReader& in, Craft& m);
bool Decode(ByteReader& in, CreatureSnapshot& m);
bool Decode(ByteReader& in, EditPlace& m);
bool Decode(ByteReader& in, EditCarve& m);
bool DecodeChunkData(ByteReader& in, glm::ivec3& coords, ChunkBlocks& blocks, ChunkLight& light);
bool Decode(ByteReader& in, ChunkUnload& m);
bool DecodeLightData(ByteReader& in, glm::ivec3& coords, ChunkLight& light);
bool Decode(ByteReader& in, BlockDeltas& m);
bool Decode(ByteReader& in, PlayerSnapshot& m);
bool Decode(ByteReader& in, Correction& m);
bool Decode(ByteReader& in, PlayerName& m);
bool Decode(ByteReader& in, Lightning& m);
bool Decode(ByteReader& in, Knockback& m);

} // namespace net
