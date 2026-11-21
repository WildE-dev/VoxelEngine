#pragma once
// Air power. Machines run on compressed air carried by air pipes; compressors make it,
// turned by water wheels through shafts.
//
//  - A water wheel is 3 x 3 blocks standing on edge around its hub (the block you
//    place), its axle level along x or z (the way you face when placing it). The eight
//    cells around the hub in the wheel's plane are its paddles: they must be clear
//    (air, water or plants), or the wheel is jammed. It turns when water flows down
//    ONE side of it: the server measures the water moving through the three paddle
//    cells on each side (ServerWorld counts every unit in and out), and the wheel
//    turns with the difference. Water pouring down one side drives it; water through
//    the middle, under it, or down both sides alike doesn't. Still water never does.
//    The faster the flow, the faster it turns, up to kWheelFullFlow.
//  - The wheel turns the shaft on its axle: shafts lie end to end along the same axis,
//    through any number of wheels' hubs, and a compressor at an end of the line takes
//    the power of every wheel on it (kWheelMaxAir a second each at full speed).
//  - Air pipes join face to face, and to compressors and machines. Everything joined
//    is one network sharing one store of air: each pipe, compressor and machine holds
//    up to kAirPerCell, and the network's pressure is how full it is (0..1).
//    Compressors stop adding air once it's full; longer networks store more but take
//    longer to fill. Breaking a pipe lets out the air it held; a network split in two
//    keeps its pressure on both sides.
//  - Machines need at least kMinPressure to run, and use air for what they do:
//     . The drill digs straight down from under itself, one block every
//       kDrillInterval seconds for kDrillAir, up to kDrillDepth deep, putting what it
//       digs into a chest touching it. It stops at water, at a chest or a machine,
//       and at anything it can't dig.
//     . A pump (see Water.h) lifts water: kPumpAir for every unit of water raised a
//       block (at least a block, even on the level). Lifting water costs half as much
//       again as wheels can win back from it falling the same height, so a pump
//       feeding its own wheels always runs down.
// The server runs it all (Server::UpdatePower, every kInterval seconds), and tells
// players near each wheel and machine how it's doing (net::Machines), to draw wheels
// and shafts turning and drills working. Right-clicking any part shows its state.
#include <cstdint>

#include "world/Block.h"

namespace Power {

constexpr double kInterval = 0.25;            // seconds between updates
constexpr float kAirPerCell = 16.0f;          // air each pipe, compressor or machine holds
constexpr float kWheelMaxAir = 8.0f;          // air a second from a wheel at full speed
constexpr float kWheelFullFlow = 256.0f;      // water units a second down one side for full speed
constexpr float kWheelMinFlow = 8.0f;         // less than this and it stands still
constexpr float kWheelSmoothing = 0.35f;      // how much of a new reading counts each update
constexpr int kMaxShaft = 64;                 // shafts and wheels in a line, at most
constexpr float kMinPressure = 0.25f;         // machines run above this
constexpr float kDrillAir = 12.0f;            // air for each block dug
constexpr double kDrillInterval = 1.5;        // seconds per block dug
constexpr int kDrillDepth = 32;               // blocks below the drill it reaches
// Air per unit of water a pump raises one block. A wheel at full speed makes
// kWheelMaxAir a second from kWheelFullFlow units a second falling past its 3-block
// side: 8 / (256 * 3) per unit-block. This is 1.5 times that.
constexpr float kPumpAir = 1.5f * kWheelMaxAir / (kWheelFullFlow * 3.0f);
constexpr int kMaxNetwork = 4096;             // pipes and machines in one network, at most

// Parts of an air network (wheels and shafts aren't: they turn compressors).
inline bool IsAirPart(BlockType t) {
    return t == BlockType::AIR_PIPE || t == BlockType::COMPRESSOR || t == BlockType::DRILL || t == BlockType::PUMP;
}
// Everything to do with air power (what right-clicking describes).
inline bool IsPart(BlockType t) { return IsAirPart(t) || t == BlockType::WATER_WHEEL || t == BlockType::SHAFT; }
// What the server keeps track of: wheels, compressors and machines (not pipes or
// shafts, which it finds from them).
inline bool IsMachine(BlockType t) {
    return t == BlockType::WATER_WHEEL || t == BlockType::COMPRESSOR || t == BlockType::DRILL || t == BlockType::PUMP;
}

// Whether a pipe (drawn as a thin tube) reaches across to this neighbour: water pipes
// join each other and pumps; air pipes join each other, compressors and machines.
inline bool PipeJoins(BlockType pipe, BlockType other) {
    if (pipe == BlockType::PIPE) return other == BlockType::PIPE || other == BlockType::PUMP;
    if (pipe == BlockType::AIR_PIPE) return IsAirPart(other);
    return false;
}

// A wheel's or a shaft's axle: along x, or along z.
inline glm::ivec3 AxleOf(const Block& b) { return b.axis == Block::kAlongZ ? glm::ivec3(0, 0, 1) : glm::ivec3(1, 0, 0); }
// Across a wheel, level (the other horizontal direction): its two sides.
inline glm::ivec3 AcrossOf(const Block& b) { return b.axis == Block::kAlongZ ? glm::ivec3(1, 0, 0) : glm::ivec3(0, 0, 1); }

// A wheel's speed (0..1) for a flow of water units a second down one side.
inline float WheelSpeed(float flow) {
    if (flow < kWheelMinFlow) return 0.0f;
    return flow >= kWheelFullFlow ? 1.0f : flow / kWheelFullFlow;
}

// What a machine is doing (sent to players, and shown when right-clicked). Wheels:
// Idle (still), Working (turning) or Blocked (jammed). Pumps: Working, NoPressure, or
// Idle (nothing to pump). Drills: any.
enum class DrillState : uint8_t { Idle, Working, NoPressure, NoChest, Blocked, Done };
inline const char* Describe(DrillState s) {
    switch (s) {
    case DrillState::Working: return "drilling";
    case DrillState::NoPressure: return "waiting for air pressure";
    case DrillState::NoChest: return "needs a chest beside it";
    case DrillState::Blocked: return "blocked";
    case DrillState::Done: return "finished (reached its depth)";
    default: return "idle";
    }
}

} // namespace Power
