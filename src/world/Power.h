#pragma once
// Air power. Machines run on compressed air carried by air pipes; water wheels make it.
//
//  - A water wheel turns when water flows past it: the water simulation's changes to
//    the six cells around the wheel are measured (ServerWorld counts every unit of
//    water that moves in or out of them). Still water doesn't turn it; a stream, a
//    waterfall, or a lake draining past it does. The faster the flow, the faster it
//    turns, up to kWheelFullFlow, and the more air it pumps (kWheelMaxAir a second).
//  - Air pipes join face to face, and to wheels and machines. Everything joined is one
//    network sharing one store of air: each pipe, wheel and machine holds up to
//    kAirPerCell, and the network's pressure is how full it is (0..1). Wheels stop
//    adding air once it's full; longer networks store more but take longer to fill.
//    Breaking a pipe lets out the air it held; a network split in two keeps its
//    pressure on both sides.
//  - Machines need at least kMinPressure to run, and use air for each thing they do.
//    The first is the drill: it digs straight down from under itself, one block every
//    kDrillInterval seconds for kDrillAir, up to kDrillDepth deep, and puts what it
//    digs into a chest touching it. It stops at water, at a chest or a machine, and at
//    anything it can't dig.
// The server runs it all (Server::UpdatePower, every kInterval seconds), and tells
// players near each wheel and machine how it's doing (net::Machines), to draw wheels
// turning and drills working. Right-clicking any part of a network shows its state.
#include <cstdint>

#include "world/Block.h"

namespace Power {

constexpr double kInterval = 0.25;            // seconds between updates
constexpr float kAirPerCell = 16.0f;          // air each pipe, wheel or machine holds
constexpr float kWheelMaxAir = 8.0f;          // air a second from a wheel at full speed
constexpr float kWheelFullFlow = 256.0f;      // water units a second past it for full speed
constexpr float kWheelMinFlow = 8.0f;         // less than this and it stands still
constexpr float kWheelSmoothing = 0.35f;      // how much of a new reading counts each update
constexpr float kMinPressure = 0.25f;         // machines run above this
constexpr float kDrillAir = 12.0f;            // air for each block dug
constexpr double kDrillInterval = 1.5;        // seconds per block dug
constexpr int kDrillDepth = 32;               // blocks below the drill it reaches
constexpr int kMaxNetwork = 4096;             // pipes and machines in one network, at most

inline bool IsPart(BlockType t) { return t == BlockType::WATER_WHEEL || t == BlockType::AIR_PIPE || t == BlockType::DRILL; }
// Wheels and machines (not pipes): what the server keeps track of.
inline bool IsMachine(BlockType t) { return t == BlockType::WATER_WHEEL || t == BlockType::DRILL; }

// A wheel's speed (0..1) for a flow of water units a second.
inline float WheelSpeed(float flow) {
    if (flow < kWheelMinFlow) return 0.0f;
    return flow >= kWheelFullFlow ? 1.0f : flow / kWheelFullFlow;
}

// What a drill is doing (sent to players, and shown when right-clicked).
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
