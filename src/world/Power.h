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
//  - Air pipes join face to face, and to compressors, machines, air tanks and valves.
//    Everything joined is one network sharing one store of air: each pipe, compressor
//    and machine holds up to kAirPerCell and each air tank kAirPerTank, and the
//    network's pressure is how full it is (0..1). A creative air tank (creative only)
//    keeps its whole network full, however much air is used. A valve joins what's either side of
//    it only while it's open; shut, the two sides are separate networks, each keeping
//    its own air.
//    Compressors stop adding air once it's full; longer networks store more but take
//    longer to fill. Breaking a pipe lets out the air it held; a network split in two
//    keeps its pressure on both sides.
//  - A pressure gauge fixed to any part of a network (it isn't one itself: it doesn't
//    join what's either side of it) shows the network's pressure on its dial.
//  - Machines need at least kMinPressure to run, and use air for what they do:
//     . The drill digs straight down from under itself, one block every
//       kDrillInterval seconds for kDrillAir, up to kDrillDepth deep, putting what it
//       digs into a chest touching it (or joined to it by item pipes). It stops at
//       water, at a chest or a machine, and at anything it can't dig.
//     . A piston (facing one of six ways; see Signals.h) pushes out while its
//       channel is on, for kPistonAir: the blocks in a row in front of it (up to
//       kPushLimit) each move a block on, and its head fills the space. A row ending
//       in water pushes the water aside (up or sideways, never lost), and plants at
//       the end are crushed. It won't move chests, beds, doors, signs, signal blocks,
//       machinery or anything unloaded. When the channel goes off it pulls its head
//       back, bringing the block in front of the head with it (free).
//     . A smelter smelts a block every kSmeltInterval seconds for kSmeltAir, from
//       the chests joined to it (touching it, or at the far end of item pipes): the
//       first thing it finds it can smelt (world/Smelting.h), the result going back
//       into the same chest. It's lit while it works.
//     . An extractor (facing; placed, it faces the way you look) takes one block or
//       item (whole) from the chest in front of it every kExtractInterval, for
//       kExtractAir, and sends it out of its back along item pipes to another chest:
//       one behind a filter pipe set to it if there is one, else one that already
//       holds some of it (so a row of chests sorts itself by example), else the
//       nearest. Things nothing can take stay put; it
//       goes through what the chest has in turn.
//     . A filter pipe passes only its type (none set: anything). Right-click one
//       with something in hand to set it to that (empty-handed: anything). Drills and
//       extractors send things only through filters that pass them; smelters and
//       crafters reach only through unset ones.
//     . A crafter makes its recipe every kCraftInterval for kCraftAir, from a chest
//       joined to it that has all it takes, putting it back in the same chest.
//       Right-click one with what you want it to make in hand to choose the recipe.
//     . A harvester reaps a ripe crop of wheat within kHarvestReach (and a block up or
//       down) every kHarvestInterval for kHarvestAir, replants it, and puts the
//       wheat in a chest joined to it.
//     . A pump (see Water.h) lifts water: kPumpAir for every unit of water raised a
//       block (at least a block, even on the level). It's cheap: the air a wheel
//       makes from water pouring past it lifts kPumpGain times that water as high as
//       the wheel (3 blocks), so a pump can keep its own wheels turning and lift
//       plenty more besides.
// The server runs it all (Server::UpdatePower, every kInterval seconds), and tells
// players near each wheel and machine how it's doing (net::Machines), to draw wheels
// and shafts turning and drills working. Right-clicking any part shows its state.
#include <cstdint>

#include "world/Block.h"
#include "world/Fluids.h"

namespace Power {

constexpr double kInterval = 0.25;            // seconds between updates
constexpr float kAirPerCell = 16.0f;          // air each pipe, compressor or machine holds
constexpr float kAirPerTank = 256.0f;         // air an air tank holds
constexpr float kWheelMaxAir = 16.0f;         // air a second from a wheel at full speed
constexpr float kWheelFullFlow = 128.0f;      // water units a second down one side for full speed (a block)
constexpr float kWheelMinFlow = 4.0f;         // less than this and it stands still
constexpr float kWheelSmoothing = 0.35f;      // how much of a new reading counts each update
constexpr int kMaxShaft = 64;                 // shafts and wheels in a line, at most
constexpr float kMinPressure = 0.25f;         // machines run above this
constexpr float kDrillAir = 12.0f;            // air for each block dug
constexpr double kDrillInterval = 1.5;        // seconds per block dug
constexpr int kDrillDepth = 32;               // blocks below the drill it reaches
// Air per unit of water a pump raises one block. A wheel at full speed makes
// kWheelMaxAir a second from kWheelFullFlow units a second falling past its 3-block
// side; that air lifts kPumpGain times the water those same 3 blocks. (One wheel at
// full speed keeps one pump going flat out, lifting 3 blocks.)
constexpr float kPumpGain = 3.0f;
constexpr float kPumpAir = kWheelMaxAir / (kWheelFullFlow * 3.0f * kPumpGain);
constexpr int kMaxNetwork = 4096;             // pipes and machines in one network, at most
constexpr float kPistonAir = 4.0f;            // air for each push (pulling back is free)
constexpr int kPushLimit = 12;                // blocks a piston can push in a row, at most
constexpr float kSmeltAir = 6.0f;             // air for each block smelted
constexpr double kSmeltInterval = 2.0;        // seconds per block smelted
constexpr int kMaxItemNetwork = 512;          // item pipes a drill or smelter looks through, at most
constexpr float kExtractAir = 1.0f;           // air for each block or item an extractor moves
constexpr double kExtractInterval = 0.5;      // seconds between its moves
constexpr float kCraftAir = 4.0f;             // air for each thing a crafter makes
constexpr double kCraftInterval = 2.0;        // seconds per thing made
constexpr float kHarvestAir = 3.0f;           // air for each crop a harvester reaps
constexpr double kHarvestInterval = 1.5;      // seconds per crop
constexpr int kHarvestReach = 4;              // blocks round a harvester it reaps (and one up or down)
constexpr float kCrushAir = 4.0f;             // air for each block a crusher grinds
constexpr double kCrushInterval = 2.0;        // seconds per block ground

// Parts of an air network (wheels and shafts aren't: they turn compressors).
// (A valve is part of the network only while it's open: shut, it divides it.)
inline bool IsAirPart(BlockType t) {
    return t == BlockType::AIR_PIPE || t == BlockType::COMPRESSOR || t == BlockType::DRILL || t == BlockType::PUMP || t == BlockType::AIR_TANK ||
           t == BlockType::CREATIVE_AIR_TANK || t == BlockType::VALVE_OPEN || t == BlockType::PISTON || t == BlockType::PRESSURE_SENSOR || t == BlockType::SMELTER || t == BlockType::SMELTER_LIT ||
           t == BlockType::EXTRACTOR || t == BlockType::CRAFTER || t == BlockType::HARVESTER || t == BlockType::ENGINE || t == BlockType::OIL_DERRICK ||
           t == BlockType::REFINERY || t == BlockType::CRUSHER || t == BlockType::TURBINE || t == BlockType::AIR_DIFFUSER;
}
// What item pipes join: each other, chests, and the machines that fill or empty them.
inline bool IsItemPart(BlockType t) {
    return t == BlockType::ITEM_PIPE || t == BlockType::FILTER_PIPE || IsStorageType(t) || t == BlockType::DRILL || t == BlockType::SMELTER ||
           t == BlockType::SMELTER_LIT || t == BlockType::EXTRACTOR || t == BlockType::CRAFTER || t == BlockType::HARVESTER || t == BlockType::CRUSHER || t == BlockType::BOILER;
}
// How much air a part holds when full.
inline float CapacityOf(BlockType t) { return t == BlockType::AIR_TANK || t == BlockType::CREATIVE_AIR_TANK ? kAirPerTank : kAirPerCell; }
// Everything to do with air power (what right-clicking describes).
inline bool IsPart(BlockType t) {
    return IsAirPart(t) || t == BlockType::WATER_WHEEL || t == BlockType::SHAFT || t == BlockType::VALVE || t == BlockType::PRESSURE_GAUGE || Fluids::IsPart(t) ||
           Fluids::IsMachine(t) || t == BlockType::MULTIBLOCK || t == BlockType::MULTIBLOCK_PART;
}
// What the server keeps track of: wheels, compressors and machines (not pipes or
// shafts, which it finds from them).
inline bool IsMachine(BlockType t) {
    return t == BlockType::WATER_WHEEL || t == BlockType::COMPRESSOR || t == BlockType::DRILL || t == BlockType::PUMP || t == BlockType::AIR_TANK ||
           t == BlockType::CREATIVE_AIR_TANK || t == BlockType::PISTON || t == BlockType::PISTON_HEAD || t == BlockType::PRESSURE_SENSOR || t == BlockType::SMELTER || t == BlockType::SMELTER_LIT ||
           t == BlockType::EXTRACTOR || t == BlockType::CRAFTER || t == BlockType::HARVESTER || t == BlockType::PRESSURE_GAUGE ||
           t == BlockType::FLUID_TANK || Fluids::IsMachine(t) || t == BlockType::MINING_POST || t == BlockType::MULTIBLOCK ||
           t == BlockType::CRUSHER || t == BlockType::AIR_DIFFUSER;
}

// Whether a pipe (drawn as a thin tube) reaches across to this neighbour: water pipes
// join each other and pumps; air pipes join each other, compressors and machines.
inline bool PipeJoins(BlockType pipe, BlockType other) {
    if (pipe == BlockType::PIPE) return other == BlockType::PIPE || other == BlockType::PUMP;
    if (pipe == BlockType::AIR_PIPE) return IsAirPart(other) || other == BlockType::VALVE;
    if (pipe == BlockType::ITEM_PIPE || pipe == BlockType::FILTER_PIPE) return IsItemPart(other);
    if (pipe == BlockType::FLUID_PIPE) return Fluids::PipeJoins(other) || other == BlockType::PUMP; // (a pump sends its water up fluid pipes)
    if (pipe == BlockType::OBSIDIAN_PIPE) return Fluids::PipeJoins(other);
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
enum class DrillState : uint8_t { Idle, Working, NoPressure, NoChest, Blocked, Done, Full };
inline const char* Describe(DrillState s) {
    switch (s) {
    case DrillState::Working: return "drilling";
    case DrillState::NoPressure: return "waiting for air pressure";
    case DrillState::NoChest: return "needs a chest beside it";
    case DrillState::Blocked: return "blocked";
    case DrillState::Done: return "finished (reached its depth)";
    case DrillState::Full: return "its chest is full";
    default: return "idle";
    }
}

} // namespace Power
