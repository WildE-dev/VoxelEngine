#pragma once
// Fluids in pipes: water, crude oil and fuel.
//
//  - Fluid pipes join face to face, and to fluid tanks; everything joined is one
//    network holding one fluid at a time (it takes the kind of the first fluid put in,
//    and nothing else until it's empty again). Each pipe holds kPerPipe units and each
//    tank kPerTank (units as water's: kUnitsPerBlock... a block is 128).
//  - Machines join a network from beside it (they aren't part of it, so they don't
//    join what's on either side of them):
//     . an oil derrick (an air machine) pumps crude oil up out of the oil shale under
//       it (a 3 x 3 column, down to kWellDepth below it), kDerrickUnits every
//       kDerrickInterval for kDerrickAir, into a network beside it with room for it.
//       Each shale block gives kShaleUnits in all, then it's spent (stone).
//     . a refinery (an air machine, facing the way you look when you place it) takes
//       crude from the network at its back and puts the same amount of fuel into the
//       network in front of it, up to kRefineUnits every update, for kRefineAir.
//     . an engine (part of an air network, like a compressor) burns kEngineFuel units
//       of fuel a second from a network beside it, adding kEngineAir a second to its
//       air network.
//     . a drain (world/Block.h) takes water from the cells above and beside it into a
//       network beside it, only as much as it has room for (none is lost); a sinkhole
//       (the natural drain) takes it out of the world.
//     . a fluid outlet (facing) lets water from a network beside it out into the cell
//       in front of it, as much as there's room for.
// The server runs it with the air (Server::UpdatePower), and saves what each pipe and
// tank holds with the world.
#include <cstdint>

#include "world/Block.h"

namespace Fluids {

enum class Kind : uint8_t { None, Water, Crude, Fuel, Count };

constexpr int kPerPipe = 64;
constexpr int kPerTank = 128 * 16;
constexpr int kWellDepth = 110;
constexpr int kDerrickUnits = 16;
constexpr double kDerrickInterval = 0.5;
constexpr float kDerrickAir = 2.0f;
constexpr int kShaleUnits = 255 * kDerrickUnits; // (the block's axis counts the draws)
constexpr int kRefineUnits = 32;
constexpr float kRefineAir = 2.0f;
constexpr float kEngineFuel = 16.0f;
constexpr float kEngineAir = 24.0f;

inline bool IsPart(BlockType t) { return t == BlockType::FLUID_PIPE || t == BlockType::FLUID_TANK; }
inline int CapacityOf(BlockType t) { return t == BlockType::FLUID_TANK ? kPerTank : t == BlockType::FLUID_PIPE ? kPerPipe : 0; }
// Machines that join fluid networks from beside them.
inline bool IsMachine(BlockType t) {
    return t == BlockType::OIL_DERRICK || t == BlockType::REFINERY || t == BlockType::ENGINE || t == BlockType::DRAIN || t == BlockType::FLUID_OUTLET;
}
// What a fluid pipe draws a tube to.
inline bool PipeJoins(BlockType other) { return IsPart(other) || IsMachine(other); }

inline const char* Name(Kind k) {
    switch (k) {
    case Kind::Water: return "water";
    case Kind::Crude: return "crude oil";
    case Kind::Fuel: return "fuel";
    default: return "nothing";
    }
}

} // namespace Fluids
