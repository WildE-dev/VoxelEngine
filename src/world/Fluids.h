#pragma once
// Fluids in pipes: water, crude oil, fuel and steam.
//
//  - Fluid pipes join face to face, and to fluid tanks and open fluid valves (a shut
//    one keeps the two sides apart, and closes the pipes' ends); everything joined is one
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
//       in front of it, as much as there's room for; and steam out into the air there
//       (as below).
//     . a boiler boils water from the network under it into steam in the network over
//       it, up to kBoilRate units a second, while it has fire: it burns a coal from the
//       chests joined to it every kCoalBurn seconds (a coke lasts kCokeBurn).
//     . a steam turbine (part of an air network, like an engine) uses kTurbineSteam
//       units of steam a second from a network beside it, adding kTurbineAir a second
//       to its air network. (Making one takes a forge golem's core.)
//  - Steam lets itself out into the air (server/Steam.h): out of every open end of its
//    pipes (a pipe joined to just one thing, out of its end face, the side away from
//    what it's joined to), up to kVentRate units a second each, and every fluid outlet
//    on its network, up to kOutletVentRate, into the cell there if it's open. Each unit becomes kSteamGas units
//    of steam in the air. Pipes meant to carry steam to a turbine want their ends closed.
//  - Lava: only a network of obsidian pipes alone holds it (any other part and it won't
//    take any). An obsidian pipe touching lava draws it in, kLavaIntake units a second,
//    as far as the network has room; a boiler beside a network of lava burns it
//    (kLavaBurn a second) instead of coal.
// The server runs it with the air (Server::UpdatePower), and saves what each pipe and
// tank holds with the world.
#include <cstdint>

#include "world/Block.h"

namespace Fluids {

enum class Kind : uint8_t { None, Water, Crude, Fuel, Steam, Lava, Count };

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
constexpr float kBoilRate = 32.0f;
constexpr double kCoalBurn = 8.0, kCokeBurn = 16.0;
constexpr float kTurbineSteam = 32.0f;
constexpr float kTurbineAir = 40.0f;
constexpr float kVentRate = 32.0f;       // steam out of each open pipe end, a second
constexpr float kOutletVentRate = 64.0f; // and out of each fluid outlet (the way to let it out on purpose)
constexpr int kSteamGas = 10;      // steam in the air from each unit let out
constexpr float kLavaIntake = 32.0f; // lava an obsidian pipe draws in from lava it touches, a second
constexpr float kLavaBurn = 4.0f;    // lava a boiler burns a second (instead of coal)

inline bool IsPart(BlockType t) { return t == BlockType::FLUID_PIPE || t == BlockType::FLUID_TANK || t == BlockType::FLUID_VALVE_OPEN || t == BlockType::OBSIDIAN_PIPE; }
inline int CapacityOf(BlockType t) {
    return t == BlockType::FLUID_TANK ? kPerTank : t == BlockType::FLUID_PIPE || t == BlockType::FLUID_VALVE_OPEN || t == BlockType::OBSIDIAN_PIPE ? kPerPipe : 0;
}
// Machines that join fluid networks from beside them.
inline bool IsMachine(BlockType t) {
    return t == BlockType::OIL_DERRICK || t == BlockType::REFINERY || t == BlockType::ENGINE || t == BlockType::DRAIN || t == BlockType::FLUID_OUTLET ||
           t == BlockType::BOILER || t == BlockType::TURBINE;
}
// What a fluid pipe draws a tube to.
inline bool PipeJoins(BlockType other) { return IsPart(other) || IsMachine(other) || other == BlockType::FLUID_VALVE; } // (a shut valve too: it closes the pipe)

inline const char* Name(Kind k) {
    switch (k) {
    case Kind::Water: return "water";
    case Kind::Crude: return "crude oil";
    case Kind::Fuel: return "fuel";
    case Kind::Steam: return "steam";
    case Kind::Lava: return "lava";
    default: return "nothing";
    }
}

} // namespace Fluids
