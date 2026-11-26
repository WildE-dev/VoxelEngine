#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

// Per-column heights for a partial ("sloped") block, in sixteenths of a block (a pixel
// of a block's texture): each of its 4 vertical edges (a slope's corners, a stepped
// block's quarters) is a column from a bottom height (0..15) up to a top height
// (1..16), at least a pixel tall, or empty (none: a stepped block's quarter cut away).
// Packed a byte an edge: the top's level in the high nibble (0, a pixel up, to 15, the
// top of the cell) and the bottom's in the low nibble (0..15); a bottom above the top
// is an empty column. Whole: 0xF0 on every edge.
struct EdgeData {
    static constexpr int kSteps = 16; // heights in sixteenths of a block
    static constexpr uint8_t kFull = 0xF0, kEmpty = 0x01;
    uint8_t edges[4];

    EdgeData() { MakeFull(); }

    void MakeFull() {
        for (auto& e : edges) e = kFull;
    }

    bool IsFull() const {
        for (auto e : edges)
            if (e != kFull) return false;
        return true;
    }

    bool IsEmpty(int edgeIndex) const {
        if (edgeIndex < 0 || edgeIndex >= 4) return true;
        return (edges[edgeIndex] & 0x0F) > (edges[edgeIndex] >> 4);
    }

    // A block is only valid if at most two of its edges are empty.
    bool IsValid() const {
        int empty = 0;
        for (int i = 0; i < 4; i++) empty += IsEmpty(i);
        return empty <= 2;
    }

    // Every column a sixteenth lower (one a pixel tall goes).
    void Shrink() {
        for (int i = 0; i < 4; i++) {
            const int top = GetTopY(i);
            if (top > 0) SetTopY(i, static_cast<uint8_t>(top - 1));
        }
    }

    // A column from `bottom` to `top` (sixteenths; top <= bottom: empty).
    void Set(int edgeIndex, int top, int bottom) {
        if (edgeIndex < 0 || edgeIndex >= 4) return;
        top = top < 0 ? 0 : top > kSteps ? kSteps : top;
        bottom = bottom < 0 ? 0 : bottom > kSteps ? kSteps : bottom;
        edges[edgeIndex] = top <= bottom ? kEmpty : static_cast<uint8_t>(((top - 1) << 4) | bottom);
    }
    // One end of a column, the other kept (an empty column's ends are both 0).
    void SetBottomY(int edgeIndex, uint8_t value) { Set(edgeIndex, GetTopY(edgeIndex), value); }
    void SetTopY(int edgeIndex, uint8_t value) { Set(edgeIndex, value, GetBottomY(edgeIndex)); }

    // Heights in sixteenths: the bottom 0..15, the top 1..16 (an empty column: 0 and 0).
    uint8_t GetBottomY(int edgeIndex) const {
        if (IsEmpty(edgeIndex)) return 0;
        return edges[edgeIndex] & 0x0F;
    }

    uint8_t GetTopY(int edgeIndex) const {
        if (IsEmpty(edgeIndex)) return 0;
        return static_cast<uint8_t>((edges[edgeIndex] >> 4) + 1);
    }

    bool operator==(const EdgeData& o) const {
        for (int i = 0; i < 4; i++)
            if (edges[i] != o.edges[i]) return false;
        return true;
    }

    // Saves before format 22 (and their chunks) kept eighths: top in the high nibble
    // 0..8, bottom in the low 0..8 (equal: empty). The same shape in sixteenths.
    static EdgeData FromEighths(const uint8_t old[4]) {
        EdgeData e;
        for (int i = 0; i < 4; i++) {
            const int top = old[i] >> 4, bottom = old[i] & 0x0F;
            e.Set(i, top > 8 ? kSteps : 2 * top, bottom > 8 ? kSteps : 2 * bottom);
        }
        return e;
    }
};

// Things that can stand in the world. (What's only carried is an item: world/Items.h.)
// Saved by number, in chunks and as items (format 17 on): new types go at the end,
// before COUNT. Older saves numbered them with the items among them: world/Legacy.h.
enum class BlockType : uint8_t {
    AIR,
    GRASS,
    DIRT,
    STONE,
    LAMP,
    LOG,
    LEAVES,
    // Plants: drawn as crossed sprites, no collision, don't block light.
    TALL_GRASS,
    FLOWER_RED,
    FLOWER_YELLOW,
    SAND,
    // Liquid: no collision, translucent, finite volume that flows (see Water.cpp).
    WATER,
    // Machines: a pump moves water from the cells around it through the pipes
    // connected to it, out of the pipes' open ends (see Water.cpp).
    PUMP,
    PIPE,
    // Dungeon walls (see Dungeons.cpp); also placeable.
    STONE_BRICK,
    MOSSY_BRICK,
    // Traps (see Server.cpp's UpdateTraps). Spikes hurt whoever stands in them (drawn
    // as a sprite, like plants); crumbling bricks give way soon after someone stands
    // on them; a pressure plate (a thin slab) opens every sluice gate near it.
    SPIKES,
    CRUMBLING_BRICK,
    PRESSURE_PLATE,
    SLUICE,
    // Mountain peaks.
    SNOW,
    // Biomes (see TerrainGenerator): savanna's dry grass, jungle and swamp's lush grass,
    // the frozen north's snowy grass and pines, desert cacti and dead bushes, sea ice.
    DRY_GRASS,
    LUSH_GRASS,
    SNOWY_GRASS,
    PINE_LEAVES,
    CACTUS,
    DEAD_BUSH,
    ICE,
    // A few loose stones on the ground (drawn as tiny stones, passable like a plant):
    // picked up, they're a quarter of a block of stone.
    PEBBLES,
    // Crafted (see world/Crafting.h): planks from logs; glass from sand, a full block
    // you can see through (its faces don't hide what's behind it, and light passes).
    PLANKS,
    GLASS,
    // A chest: right-click to open it and store things in it (kept by the server, see
    // Server::HandleChestMove).
    CHEST,
    // A bed (a half-height block): right-click to make it where you respawn, and at
    // night to sleep (see Server::HandleUseBed).
    BED,
    // Ores, in veins in the stone underground (iron deeper): coal ore breaks into
    // coal; iron ore is smelted into iron ingots (see world/Crafting.h).
    COAL_ORE,
    IRON_ORE,
    // Farming: seeds (an item, from tall grass) planted on dirt or grass grow into a
    // crop that ripens into wheat (the server grows them: Server::UpdateCrops).
    CROP,       // plant: young
    RIPE_WHEAT, // plant: ready to harvest
    // A ladder: a see-through frame of rungs you walk into and climb (passable like a
    // plant; drawn as a cube).
    LADDER,
    // A door, two cells tall: closed it's solid; right-click opens it (both halves) into
    // an open frame you walk through (passable, drawn like a ladder's shell).
    DOOR,
    DOOR_OPEN,
    // A campfire: a bright light (drawn as a sprite of logs and flames, passable),
    // smoking; right-click it with raw food in hand to cook it; standing in it burns.
    CAMPFIRE,
    // A sign on a post (a sprite, passable) with a line of text on it, written after
    // placing it (or rewritten with a right-click); the server keeps the texts.
    SIGN,
    // TNT: right-click to light it; three seconds later it explodes (the server runs it).
    TNT,
    TNT_LIT, // (flashing, about to go)
    // Saplings (plants, from leaves): planted on soil, they grow into a tree.
    SAPLING,
    PINE_SAPLING,
    // Air power (see world/Power.h): a water wheel (3 x 3, around its hub block) turned
    // by water flowing down one side turns the shaft on its axle; a compressor at the
    // shaft's end pumps air into the air pipes joined to it; machines on the pipes run
    // on the pressure. Wheels and shafts are drawn as turning models, not cubes.
    WATER_WHEEL,
    AIR_PIPE,
    DRILL, // a machine: digs straight down, into a chest beside it
    SHAFT,      // carries a wheel's turning along its axle (drawn as a turning rod)
    COMPRESSOR, // turned by a shaft, pumps air into the air pipes joined to it
    // Signals (see world/Signals.h): wireless, on numbered channels. A lever (a sprite,
    // passable) sends while it's on; a logic gate works out a channel from others; a
    // signal lamp lights while its channel is on.
    LEVER,
    LEVER_ON,
    LOGIC_GATE,
    SIGNAL_LAMP,
    SIGNAL_LAMP_ON,
    // Air power: a tank holds a lot of air (a store of pressure); a valve joins the air
    // pipes either side of it only while it's open (by hand, or by a signal).
    AIR_TANK,
    VALVE,
    VALVE_OPEN,
    // A piston (an air machine listening to a channel): while its channel is on it
    // pushes the blocks in front of it out a block and holds them there with its head
    // (an arm, drawn thin); when it goes off it pulls the head back, and the block in
    // front of it with it. It faces one of six ways (see Block::IsFacingType).
    PISTON,
    PISTON_HEAD,
    // Sensors: send on their channel while there's water touching them, while the air
    // network they're on is at a set pressure, by day, or while a player is near.
    WATER_SENSOR,
    PRESSURE_SENSOR,
    DAYLIGHT_SENSOR,
    PLAYER_SENSOR,
    // Item pipes (drawn as thin tubes) join chests to drills and smelters further away.
    // A smelter (an air machine; lit while it works) smelts what's in the chests joined
    // to it, in place (see world/Smelting.h).
    ITEM_PIPE,
    SMELTER,
    SMELTER_LIT,
    // A spring: wet rock that water wells up out of, without end (into the cells above
    // and beside it: see Water.h). Found where lakes overflow, starting the streams that
    // run down from them. Breaking it gives stone, and the spring is gone.
    SPRING,
    // Automation (air machines; see world/Power.h). An extractor (facing, like a
    // piston) takes things out of the chest in front of it and sends them out of its
    // back along item pipes to other chests. A filter pipe is an item pipe that lets
    // only one kind through (kept in its `axis`: see Items::FilterOf). A crafter makes
    // a recipe (chosen, kept in its `axis`) from what's in a chest joined to it. A
    // harvester reaps ripe wheat around it, replants it, and puts the wheat in a chest.
    EXTRACTOR,
    FILTER_PIPE,
    CRAFTER,
    HARVESTER,
    // A drain: an iron grate that water goes down and is gone (taking from the cells
    // above and beside it: see Water.h). Crafted, to build with.
    DRAIN,
    // A sinkhole: the natural drain, a dark opening in wet rock where rivers end (it
    // works just like a drain). Breaking it gives stone, and it's gone.
    SINKHOLE,
    // A pressure gauge: a dial fixed to an air pipe (or a tank, compressor or any other
    // part of an air network), facing away from it, that shows the network's pressure.
    // A facing block (its axis: the way the dial faces), drawn as a model.
    PRESSURE_GAUGE,
    // Oil (see world/Fluids.h). Oil shale: rock soaked in crude oil, in pockets deep
    // underground; an oil derrick above pumps it out (its `axis` counts how much has
    // been taken: spent, it's stone). Tar: a dark seep on the ground over a pocket.
    OIL_SHALE,
    TAR,
    // Fluid pipes (drawn as thin tubes) and tanks carry and hold one fluid at a time:
    // water, crude oil or fuel. The machines join them from beside: an oil derrick
    // (pumps crude up out of the shale under it), a refinery (crude in at its back,
    // fuel out of its front), an engine (burns fuel to make compressed air, like a
    // compressor), and a fluid outlet (lets water out of its face). A drain sends the
    // water it takes into the pipes.
    FLUID_PIPE,
    FLUID_TANK,
    OIL_DERRICK,
    REFINERY,
    ENGINE,
    FLUID_OUTLET,
    // A mining post: a chest that hires dwarf miners. Put cooked food in it (their
    // pay) and up to two dwarves come to work it, digging the ore within reach of it
    // and putting it in the post (see Server::UpdateMiningPosts).
    MINING_POST,
    // Multiblock structures (see world/Multiblocks.h): building blocks for them (steel
    // frames, machine casings, fire bricks; the engineer's hammer that forms them is an
    // item); and what a formed structure is made of: its core (drawn as the whole
    // structure's model) and its other parts (solid, drawn as nothing).
    STEEL_FRAME,
    MACHINE_CASING,
    FIRE_BRICK,
    MULTIBLOCK,
    MULTIBLOCK_PART,
    // A crusher (an air machine) grinds ore from the chests joined to it: an iron ore
    // into two crushed iron (each smelting to two ingots), stone into sand. Crushed
    // iron and coke (baked from coal in a coke oven: see world/Multiblocks.h) are items.
    CRUSHER,
    // The Deep (world/TerrainGenerator.h): glowing fungus on the floors of its caverns,
    // magma (glowing, and it burns whoever stands on it), and gold ore in its rock,
    // which smelts to gold ingots (an item: what dwarf traders pay and take).
    GLOW_FUNGUS,
    MAGMA,
    GOLD_ORE,
    // A machine sensor (world/Signals.h): sends while the machine, chest, tank or air
    // network it touches is as it's set to watch for.
    MACHINE_SENSOR,
    // Steam power (world/Fluids.h): a boiler (water in below, steam out above, burning
    // coal from its chests) and a steam turbine (steam into compressed air).
    BOILER,
    TURBINE,
    // An air diffuser (part of an air network): uses a little compressed air to make
    // the air round it breathable, deep down or high up (world/Breathing.h).
    AIR_DIFFUSER,
    // Copper: ore in the rock (commoner and shallower than iron), smelted (or worked by
    // hand) into copper ingots, the wiring every signal block needs (world/Signals.h).
    COPPER_ORE,
    // A creative air tank: part of an air network that never runs out of air (it
    // keeps the whole network full). Creative only: there's no recipe, survival
    // players can't place one, and breaking one in survival gives nothing.
    CREATIVE_AIR_TANK,
    // Lava (world/Lava.h): a glowing liquid that flows slowly, a few blocks from its
    // source, and burns whoever wades in. Water touching it boils away into steam
    // (server/Steam.h). Its `axis` holds how far it has flowed (0 a source, 1..3)
    // and whether it's falling. Creative only.
    LAVA,
    // A fluid valve: joins the fluid pipes either side of it only while it's open (like
    // the air valve with air pipes); shut, the two sides are separate networks.
    FLUID_VALVE,
    FLUID_VALVE_OPEN,
    // Obsidian: what lava cools into when water pours onto it (world/Lava.h). Very hard
    // to mine, and nothing breaks it: TNT doesn't, and raiders can't hack through it.
    OBSIDIAN,
    // An obsidian pipe: a fluid pipe that can hold lava (world/Fluids.h), drawing it in
    // where it touches lava, to fire a boiler.
    OBSIDIAN_PIPE,
    // Fire (world/Fire.h): spreads through what burns, burns whoever's in it, hard to put
    // out. Drawn by the game as flames. Creative only (it starts itself well enough).
    FIRE,
    // An air launcher (world/Power.h): facing (the way you look placing it), part of an
    // air network; each time its channel comes on it throws whatever's in front of it
    // (players, creatures, items) hard that way, for kLaunchAir.
    LAUNCHER,
    // A hay bale: soft to land on (no fall damage), and it burns readily.
    HAY_BALE,
    // A heat sensor (world/Signals.h): sends while fire or lava is near it.
    HEAT_SENSOR,
    // A sprinkler (world/Fluids.h): on fluid pipes of water, it sprays the fires
    // round and below it out, while its channel is on (with none, whenever it
    // senses fire itself).
    SPRINKLER,
    // Asphalt: bitumen (an item, from a refinery) and stone, a road: you walk faster on it (kAsphaltPace).
    ASPHALT,
    // Limestone: beds of it in the rock. Burnt (a smelter, or by hand with coal) it's
    // cement (an item); cement, gravel and sand make concrete, a tough building block.
    LIMESTONE,
    CONCRETE,
    // Gravel: pockets of it under the topsoil; it goes into asphalt and concrete.
    GRAVEL,
    // Reinforced concrete: concrete with steel bars through it. Takes a blast, like
    // obsidian, and is slow to break.
    REINFORCED_CONCRETE,
    // Deepstone: the rock below the lava level (the Deep and under it), tougher than stone.
    DEEPSTONE,
    // A grave: where a player died, holding everything they had (opened like a chest).
    // Only made then: it isn't carried, fire and blasts leave it be, and once it's
    // emptied it crumbles away.
    GRAVE,
    COUNT
};
// Machines with an inventory of their own (opened like a chest, empty-handed): what
// they work from and put what they make in, before any chest joined to them.
inline bool IsMachineInventory(BlockType t) {
    return t == BlockType::SMELTER || t == BlockType::SMELTER_LIT || t == BlockType::DRILL || t == BlockType::CRAFTER || t == BlockType::HARVESTER ||
           t == BlockType::CRUSHER || t == BlockType::BOILER || t == BlockType::MULTIBLOCK;
}
// Blocks only creative players may place (and that give nothing when broken).
// The old water pipe (BlockType::PIPE), from before fluid pipes carried pumps' water:
// kept for its number, and turned into a fluid pipe wherever it's found.
inline BlockType Current(BlockType t) { return t == BlockType::PIPE ? BlockType::FLUID_PIPE : t; }
// Valves (air and fluid, shut and open): drawn as a length of their pipe with a handle
// on top, turned along the pipe when open and across it when shut.
inline bool IsValveType(BlockType t) {
    return t == BlockType::VALVE || t == BlockType::VALVE_OPEN || t == BlockType::FLUID_VALVE || t == BlockType::FLUID_VALVE_OPEN;
}
inline bool IsOpenValve(BlockType t) { return t == BlockType::VALVE_OPEN || t == BlockType::FLUID_VALVE_OPEN; }
// The same valve opened (or shut).
inline BlockType ValveSetTo(BlockType t, bool open) {
    const bool fluid = t == BlockType::FLUID_VALVE || t == BlockType::FLUID_VALVE_OPEN;
    return fluid ? (open ? BlockType::FLUID_VALVE_OPEN : BlockType::FLUID_VALVE) : (open ? BlockType::VALVE_OPEN : BlockType::VALVE);
}
// The pipe a valve sits in (whose tube it's drawn as, and joins like).
inline BlockType ValvePipe(BlockType t) { return t == BlockType::FLUID_VALVE || t == BlockType::FLUID_VALVE_OPEN ? BlockType::FLUID_PIPE : BlockType::AIR_PIPE; }
inline bool IsCreativeOnlyType(BlockType t) { return t == BlockType::CREATIVE_AIR_TANK || t == BlockType::LAVA || t == BlockType::FIRE || t == BlockType::GRAVE; }
// Blocks that are only ever part of something bigger: a piston's head, and a formed
// structure's core and parts. They're made and unmade with it, never carried or put
// down as themselves, and worth nothing by themselves (breaking one gives what it's part
// of: Items::ResourceOf, Block::ResourceUnits, BlockAccess::CanPlace).
inline bool IsPartOnlyType(BlockType t) { return t == BlockType::PISTON_HEAD || t == BlockType::MULTIBLOCK || t == BlockType::MULTIBLOCK_PART; }
// Blocks that hold things like a chest (opened, filled, emptied, broken open).
inline bool IsStorageType(BlockType t) { return t == BlockType::CHEST || t == BlockType::MINING_POST || t == BlockType::GRAVE || IsMachineInventory(t); }
// Storage that item pipes and machines join (not a grave: what's in it is for its owner to fetch).
inline bool IsPipedStorageType(BlockType t) { return IsStorageType(t) && t != BlockType::GRAVE; }

// Solid blocks drawn by a model of their own (turning wheels and shafts), and pipes
// drawn as thin tubes: neither hides what's beside it or blocks light.
inline bool IsLeverType(BlockType t) { return t == BlockType::LEVER || t == BlockType::LEVER_ON; }
inline bool IsModelType(BlockType type) {
    return type == BlockType::WATER_WHEEL || type == BlockType::SHAFT || type == BlockType::PRESSURE_GAUGE || type == BlockType::MULTIBLOCK ||
           type == BlockType::MULTIBLOCK_PART || type == BlockType::DOOR || type == BlockType::DOOR_OPEN || // (a door swings open: drawn as it turns)
           IsLeverType(type) || // (a lever's handle tips over)
           type == BlockType::FIRE; // (flames)
}
// Ladders and doors face one of the four level ways (their `axis`, a Face: +x, -x, +z or
// -z): a ladder out from the wall it hangs on, a door toward whoever put it up. Each is a
// thin panel at the back of its cell (a door's hinge at its left, seen from the front).
inline bool IsWallFacingType(BlockType type) { return type == BlockType::LADDER || type == BlockType::DOOR || type == BlockType::DOOR_OPEN; }
inline bool IsPipeType(BlockType type) {
    return type == BlockType::PIPE || type == BlockType::AIR_PIPE || type == BlockType::ITEM_PIPE || type == BlockType::FILTER_PIPE || type == BlockType::FLUID_PIPE ||
           type == BlockType::OBSIDIAN_PIPE;
}
// Blocks that face one of six ways (their `axis` holds the Face they point to): a
// piston and its head.
// Water goes down these and is gone (see Water.h).
inline bool IsDrainType(BlockType type) { return type == BlockType::DRAIN || type == BlockType::SINKHOLE; }
inline bool IsFacingType(BlockType type) {
    return type == BlockType::PISTON || type == BlockType::PISTON_HEAD || type == BlockType::EXTRACTOR || type == BlockType::PRESSURE_GAUGE ||
           type == BlockType::LAUNCHER ||
           type == BlockType::REFINERY || type == BlockType::FLUID_OUTLET;
}

inline bool IsPlantType(BlockType type) {
    return type == BlockType::TALL_GRASS || type == BlockType::FLOWER_RED || type == BlockType::FLOWER_YELLOW || type == BlockType::SPIKES ||
           type == BlockType::DEAD_BUSH || type == BlockType::PEBBLES || type == BlockType::CROP || type == BlockType::RIPE_WHEAT ||
           type == BlockType::LADDER || type == BlockType::DOOR_OPEN || type == BlockType::CAMPFIRE ||
           type == BlockType::SIGN || type == BlockType::SAPLING || type == BlockType::PINE_SAPLING || type == BlockType::LEVER ||
           type == BlockType::LEVER_ON || type == BlockType::GLOW_FUNGUS || type == BlockType::FIRE;
}

// Grass of any biome (plants grow on it).
inline bool IsGrassType(BlockType type) {
    return type == BlockType::GRASS || type == BlockType::DRY_GRASS || type == BlockType::LUSH_GRASS || type == BlockType::SNOWY_GRASS;
}
// Leaves of any tree: full blocks that let light through.
inline bool IsLeavesType(BlockType type) { return type == BlockType::LEAVES || type == BlockType::PINE_LEAVES; }

constexpr uint8_t kMaxLight = 15;

// Light from emitting blocks, and daylight from the sky. Stored and propagated
// separately so day/night can scale sky light in the shader without re-meshing.
enum class LightChannel : uint8_t { Block, Sky };

// How a partial block's shape is drawn (and stood on): natural ground slopes smoothly
// between its four corner heights; anything built is four quarter-columns, each as
// high as its corner (like small steps).
enum class ShapeStyle : uint8_t { Sloped, Stepped };
inline ShapeStyle StyleOf(BlockType type) {
    switch (type) {
    case BlockType::GRASS:
    case BlockType::DIRT:
    case BlockType::SAND:
    case BlockType::TAR:
    case BlockType::STONE:
    case BlockType::DEEPSTONE:
    case BlockType::SNOW:
    case BlockType::DRY_GRASS:
    case BlockType::LUSH_GRASS:
    case BlockType::SNOWY_GRASS: return ShapeStyle::Sloped;
    default: return ShapeStyle::Stepped;
    }
}

// Block light emitted by a block type, 0..kMaxLight.
// (The game's own; Resources/Data/blocks.json may change it: see world/BlockData.h.)
inline uint8_t DefaultLightEmission(BlockType type) {
    return type == BlockType::LAMP || type == BlockType::SIGNAL_LAMP_ON ? kMaxLight : type == BlockType::CAMPFIRE ? kMaxLight - 1 : type == BlockType::SMELTER_LIT ? 12
         : type == BlockType::GLOW_FUNGUS ? 11 : type == BlockType::MAGMA ? 12 : type == BlockType::LAVA || type == BlockType::FIRE ? kMaxLight : 0;
}
namespace BlockData { extern std::atomic<const int8_t*> gLight; }
inline uint8_t GetLightEmission(BlockType type) {
    const int8_t set = BlockData::gLight.load(std::memory_order_acquire)[static_cast<size_t>(type)];
    return set >= 0 ? static_cast<uint8_t>(set) : DefaultLightEmission(type);
}

// Indices into the texture atlas for each face of a block.
struct TextureData {
    int top, bottom, front, back, left, right;

    constexpr TextureData() : top(0), bottom(0), front(0), back(0), left(0), right(0) {}
    constexpr explicit TextureData(int all) : top(all), bottom(all), front(all), back(all), left(all), right(all) {}
    constexpr TextureData(int top, int bottom, int front, int back, int left, int right)
        : top(top), bottom(bottom), front(front), back(back), left(left), right(right) {}
};

// A short id for a block type ("dirt", "stone_brick"), for commands; and back
// (BlockType::COUNT if there's no such id).
const char* BlockId(BlockType type);
BlockType BlockFromId(const std::string& id);

// The game's own texture for a face (0 top, 1 bottom, 2 front, 3 back, 4 left,
// 5 right), or nullptr (Resources/Data/blocks.json may name another).
const char* DefaultBlockTexture(BlockType type, int face);
// Atlas tile indices for each face of a block type. Thread-safe.
TextureData GetTextureData(BlockType type);
// Atlas tile a block shows where it's been carved (chiselled faces inside its cell),
// or -1 if it looks the same inside (only logs differ: wood under the bark).
int GetCarvedTexture(BlockType type);

// Maps the texture names each block type uses (see Block.cpp) to atlas indices.
// Call after (re)building the atlas, then rebuild chunk meshes. Safe to call while
// the world worker is meshing.
void ResolveBlockTextures(const std::function<int(const std::string&)>& lookup);

// Face order is shared by the mesher, the vertex generator and the neighbour lookup.
enum class Face : int {
    Right = 0,  // +X
    Left = 1,   // -X
    Top = 2,    // +Y
    Bottom = 3, // -Y
    Back = 4,   // +Z
    Front = 5,  // -Z
};

constexpr int kFaceCount = 6;
constexpr int kFaceOffsets[kFaceCount][3] = {
    { 1,  0,  0},
    {-1,  0,  0},
    { 0,  1,  0},
    { 0, -1,  0},
    { 0,  0,  1},
    { 0,  0, -1},
};

// Axis (0 = x, 1 = y, 2 = z) each face points along, and the two tangent axes
// spanning it. Used to address the four corners of a face for ambient occlusion.
constexpr int kFaceAxis[kFaceCount] = { 0, 0, 1, 1, 2, 2 };
constexpr int kFaceTangents[3][2] = { {1, 2}, {0, 2}, {0, 1} };

// Per-corner shading for one face, indexed [high side of tangent A][high side of tangent B].
struct FaceShading {
    uint8_t ao[2][2] = { {3, 3}, {3, 3} };    // 0 (darkest) to 3 (unoccluded)
    uint8_t light[2][2] = { {0, 0}, {0, 0} }; // block light, 0..kMaxLight
    uint8_t sky[2][2] = { {0, 0}, {0, 0} };   // sky light, 0..kMaxLight
};

struct Block {
    BlockType type = BlockType::AIR;
    EdgeData edgeData;
    // Water in this cell, 0..kMaxWaterVolume units: a water block's volume, or water
    // filling the space above a partial block's slope ("waterlogged").
    uint8_t water = 0;
    // The volume as drawn. While water is moving it only follows `water` once that has
    // drifted kShownDeadband away, which hides the small back-and-forth of flowing water
    // (otherwise visible as jitter while a lake drains); once the cell is at rest the
    // water simulation snaps it to the true volume. Below kShownDeadband it follows
    // exactly, so the last thin layer of a draining lake thins out smoothly instead of
    // holding at an eighth and then vanishing. Rendering only.
    uint8_t shown = 0;
    // Which way an orientable block (a log) runs: kUpright, kAlongX or kAlongZ. Its
    // ends face that way and its grain runs along it. A facing block (a piston) keeps
    // the Face it points to here (0..5) instead; a filter pipe what it lets through
    // (Items::FilterOf: 0, anything); a crafter its recipe (index + 1; 0: none). Always 0
    // for others.
    uint8_t axis = 0;
    static constexpr uint8_t kUpright = 0, kAlongX = 1, kAlongZ = 2;
    static constexpr uint8_t kMaxAxis = 5; // (a facing block's Face)
    // A facing block's direction (a unit step).
    glm::ivec3 Facing() const {
        const int f = (axis & 7) % 6;
        return glm::ivec3(kFaceOffsets[f][0], kFaceOffsets[f][1], kFaceOffsets[f][2]);
    }
    // A door's hinge: on the left of the way it faces, or (mirrored, this bit of its axis)
    // on the right. Placing one, kDoorMirrored in the face axis asks for it.
    static constexpr uint8_t kDoorMirrored = 8;
    bool DoorMirrored() const { return (axis & kDoorMirrored) != 0; }
    // A lever is fixed to the face of a solid block and sticks out from it: the way it
    // points (out of that face). Kept in `axis` so that 0 is a lever on the floor (all
    // the old ones were): the Face is (axis + 2) % 6. The block it's fixed to is behind it.
    glm::ivec3 LeverFacing() const {
        const int f = (axis + 2) % 6;
        return glm::ivec3(kFaceOffsets[f][0], kFaceOffsets[f][1], kFaceOffsets[f][2]);
    }
    // The way it points, which its collision boxes turn to: a lever out of its face, a
    // log or shaft along its grain, anything else the way it faces.
    glm::ivec3 Pointing() const {
        if (IsLeverType(type)) return LeverFacing();
        if (IsOrientable(type)) {
            glm::ivec3 d(0);
            d[GrainAxis()] = 1;
            return d;
        }
        return Facing();
    }
    // Whether things bump into it (its "collision" says; else anything but plants,
    // liquids and air), and whether things stand on it ("floor").
    bool Collides() const;
    bool StoodOn() const;
    static uint8_t LeverAxisFor(int face) { return static_cast<uint8_t>((face + 4) % 6); }
    // Whether a block holds a lever up: a solid, whole block (not a pipe, valve or model).
    static bool HoldsLever(const Block& b) { return b.IsFullBlock() && !IsModelType(b.type) && !IsPipeType(b.type) && !IsValveType(b.type); }
    static bool IsOrientable(BlockType t) { return t == BlockType::LOG || t == BlockType::WATER_WHEEL || t == BlockType::SHAFT; }
    // The axis in kFaceAxis terms (0 x, 1 y, 2 z), and the orientation for one.
    // (A log's axis byte also holds its shape's direction, above the grain: see ShapeDir.)
    int GrainAxis() const { return (axis & 7) == kAlongX ? 0 : (axis & 7) == kAlongZ ? 2 : 1; }
    static uint8_t AxisFor(int faceAxis) { return faceAxis == 0 ? kAlongX : faceAxis == 2 ? kAlongZ : kUpright; }
    // The same block turned to run along `faceAxis` (if it can be). A facing block
    // takes a Face (0..5) instead.
    Block Turned(int faceAxis) const {
        Block b = *this;
        if (IsWallFacingType(type)) {
            b.axis = static_cast<uint8_t>(faceAxis == 0 || faceAxis == 1 || faceAxis == 4 || faceAxis == 5 ? faceAxis : 4); // (level only)
            return b;
        }
        if (IsFacingType(type)) {
            b.axis = static_cast<uint8_t>(faceAxis >= 0 && faceAxis <= kMaxAxis ? faceAxis : 2);
            return b;
        }
        b.axis = IsOrientable(type) ? AxisFor(faceAxis) : kUpright;
        // Axles are level: along x unless placed along z.
        if ((type == BlockType::WATER_WHEEL || type == BlockType::SHAFT) && b.axis == kUpright) b.axis = kAlongX;
        return b;
    }

    Block() = default;
    explicit Block(BlockType t) : type(t), water(t == BlockType::WATER ? kMaxWaterVolume : 0), shown(water) {}
    // Partial block. Falls back to air if the edge data is degenerate.
    Block(BlockType t, EdgeData edges) : type(t) { SetEdgeData(edges); }

    // A plant whose sprite is drawn `dropSteps` sixteenths (0..15) below its cell's floor, so it
    // stands on a sloped block underneath. Stored in the otherwise unused edge data.
    // A pressure plate: a slab an eighth of a block high (two sixteenths).
    static Block Bed() {
        EdgeData edges;
        for (int e = 0; e < 4; e++) edges.SetTopY(e, EdgeData::kSteps / 2); // half a block high
        return Block(BlockType::BED, edges);
    }
    // A grave: a headstone across the back half (-z), a low mound in front of it.
    static Block Grave() {
        EdgeData edges;
        edges.SetTopY(0, 4), edges.SetTopY(3, 4); // (the front quarters, z = 1: a quarter high)
        return Block(BlockType::GRAVE, edges);
    }
    static Block PressurePlate() {
        EdgeData edges;
        for (int e = 0; e < 4; e++) edges.SetTopY(e, 2);
        return Block(BlockType::PRESSURE_PLATE, edges);
    }
    // What placing a block of this type puts in the world (a full cell of water, a
    // plate's slab, a sprite standing on the floor below). Blocks that can be shaped
    // may be placed lower than full: a slab `heightSteps` sixteenths high (1..15; 16 is whole).
    // Orientable blocks run along `faceAxis` (0 x, 1 y, 2 z): the axis of the face
    // they were placed against.
    static Block Placed(BlockType t, int heightSteps = EdgeData::kSteps, int faceAxis = 1);
    // Whether a type can be placed as a slab (and chiselled): building blocks, not
    // liquids, plants or machines.
    static bool CanShape(BlockType t) { return Block(t).CanChisel(); }

    // Worth (survival mode, see world/Inventory.h): a full block is worth kUnitsPerBlock
    // units of its type; a shaped one is worth its volume (a half slab 32, each sixteenth of a
    // quarter 1). Things that can't be shaped (plants, machines) are worth a full block.
    static constexpr int kUnitsPerBlock = 64;
    static constexpr int kPebbleUnits = 16; // a quarter of a block of stone
    int ResourceUnits() const;

    static Block Plant(BlockType t, uint8_t dropSteps) {
        Block b(t);
        b.edgeData.edges[0] = static_cast<uint8_t>(dropSteps > 15 ? 15 : dropSteps);
        return b;
    }
    bool IsPlant() const { return IsPlantType(type); }
    float PlantDrop() const { return (edgeData.edges[0] & 0x0F) / static_cast<float>(EdgeData::kSteps); }

    // Water volumes are in units: a full cell holds 128, so surfaces can level to within
    // a small fraction of a block.
    static constexpr uint8_t kMaxWaterVolume = 128;
    static Block Water(uint8_t volume) {
        Block b(BlockType::WATER);
        b.water = b.shown = volume;
        return b;
    }
    // An eighth of a block: smaller back-and-forth is hidden while water moves.
    static constexpr uint8_t kShownDeadband = kMaxWaterVolume / 8;
    // A cell of only water (as opposed to a waterlogged partial block).
    bool IsLiquid() const { return type == BlockType::WATER; }
    // Lava: a liquid too, but not water's (nothing to stand on or bump into, and not
    // aimed at or broken; see world/Lava.h).
    bool IsLava() const { return type == BlockType::LAVA; }
    bool HasWater() const { return water > 0; }
    uint8_t WaterVolume() const { return water; }
    bool IsFullWater() const { return IsLiquid() && water == kMaxWaterVolume; }
    // A sloped block: solid below its surface, open above it.
    bool IsPartial() const { return !IsAir() && !IsPlant() && !IsLiquid() && !IsLava() && !IsFullBlock(); }
    // (A turned shape holds water above what it fills of each quarter: see WaterEdges.)
    bool CanHoldWater() const { return IsReplaceableByWater() || IsLiquid() || IsPartial(); }

    // Which way a shaped block's shape runs (its `axis`): 0 upright (its corners' heights
    // up from its bottom, as it always was), 1 and 2 along +x and +z (as a log's axis), 3
    // upside down (down from its top), 4 and 5 along -x and -z. The shape is kept as if
    // upright and turned into place wherever it's drawn, collided with or aimed at.
    // Chiselling a full block on a side (or underneath) turns it that way first. A log
    // keeps its grain in the byte's low three bits and the direction in the next three
    // (KeepsGrain), so it turns too, its grain running the way it did.
    static bool CanTurnShape(BlockType t) { return CanShape(t) && !IsFacingType(t) && !IsWallFacingType(t); }
    static bool KeepsGrain(BlockType t) { return IsOrientable(t) && CanShape(t); }
    int ShapeDir() const {
        if (!CanTurnShape(type)) return 0;
        const int d = KeepsGrain(type) ? axis >> 3 : axis;
        return d <= 5 ? d : 0;
    }
    void SetShapeDir(int d) {
        if (CanTurnShape(type)) axis = static_cast<uint8_t>(KeepsGrain(type) ? (axis & 7) | (d << 3) : d);
    }
    // The block as its own upright shape (ShapeDir 0), for drawing it before it's turned
    // into place: a log's grain turned along with it.
    Block Upright() const;
    // The solid a partial block fills, in the cell (0..1 each way), turned the way it's
    // turned: whether a box overlaps it (touching doesn't count), and the highest point of
    // it over a footprint (x0..x1, z0..z1; -1 if none of it is over that footprint). Exact
    // for quarters and slopes alike.
    bool ShapeOverlaps(const glm::vec3& lo, const glm::vec3& hi) const;
    // A block drawn smaller than its cell (a pipe, a shaft, a wheel's hub, a pressure
    // gauge, a piston's head, a door): the boxes (0..1 in the cell, up to kMaxModelBoxes)
    // roughly matching how it's drawn, which it collides (if solid), is stood on and is
    // aimed at as, from its "collision" in Resources/Data/blocks (see BlockData.h),
    // turned the way it points. -1 for a block with none. `joined`: the faces a pipe
    // reaches out of (ModelBoxes::Joins).
    static constexpr int kMaxModelBoxes = 8;
    int ModelBoxes(unsigned joined, glm::vec3 lo[kMaxModelBoxes], glm::vec3 hi[kMaxModelBoxes]) const;
    float ShapeTop(float x0, float x1, float z0, float z1) const;
    // The highest point of an upright slope's surface over a footprint (the footprint
    // clipped to the cell; -1 if it misses the cell).
    float SurfaceMax(float x0, float x1, float z0, float z1) const;
    // What water sees of a partial block: four quarter-columns (WaterStepped) or an
    // upright slope's corners, heights in sixteenths. A turned shape is its quarters each as
    // high as the most it fills of them.
    EdgeData WaterEdges() const;
    bool WaterStepped() const { return IsStepped() || ShapeDir() != 0; }
    // A pebbles block's stones (up to 5), in the cell (their bottom on the ground the
    // plant drop says), placed by the cell's position in its chunk.
    int PebbleStones(int x, int y, int z, glm::vec3 lo[5], glm::vec3 hi[5]) const;
    // The direction whose top faces the way `n` points (a face's normal).
    static int ShapeDirFacing(const glm::ivec3& n) {
        if (n.x > 0) return 1;
        if (n.z > 0) return 2;
        if (n.y < 0) return 3;
        if (n.x < 0) return 4;
        if (n.z < 0) return 5;
        return 0;
    }
    // A point in the cell (0..1 each way) from the shape's own upright frame, and back;
    // and a direction (no offset) likewise.
    static glm::vec3 ShapeToCell(int dir, const glm::vec3& p) {
        switch (dir) {
        case 1: return { p.y, 1.0f - p.x, p.z };
        case 2: return { p.x, 1.0f - p.z, p.y };
        case 3: return { p.x, 1.0f - p.y, 1.0f - p.z };
        case 4: return { 1.0f - p.y, p.x, p.z };
        case 5: return { p.x, p.z, 1.0f - p.y };
        default: return p;
        }
    }
    static glm::vec3 CellToShape(int dir, const glm::vec3& p) {
        switch (dir) {
        case 1: return { 1.0f - p.y, p.x, p.z };
        case 2: return { p.x, p.z, 1.0f - p.y };
        case 3: return { p.x, 1.0f - p.y, 1.0f - p.z };
        case 4: return { p.y, 1.0f - p.x, p.z };
        case 5: return { p.x, 1.0f - p.z, p.y };
        default: return p;
        }
    }
    static glm::vec3 ShapeVectorToCell(int dir, const glm::vec3& v) {
        switch (dir) {
        case 1: return { v.y, -v.x, v.z };
        case 2: return { v.x, -v.z, v.y };
        case 3: return { v.x, -v.y, -v.z };
        case 4: return { -v.y, v.x, v.z };
        case 5: return { v.x, v.z, -v.y };
        default: return v;
        }
    }
    static glm::vec3 CellVectorToShape(int dir, const glm::vec3& v) {
        switch (dir) {
        case 1: return { -v.y, v.x, v.z };
        case 2: return { v.x, v.z, -v.y };
        case 3: return { v.x, -v.y, -v.z };
        case 4: return { v.y, -v.x, v.z };
        case 5: return { v.x, -v.z, v.y };
        default: return v;
        }
    }
    // The quarter (EdgeData order) a chisel aimed at point `p` in the cell (0..1 each way)
    // works on, for a shape turned `dir` (its quarters are across its own upright frame).
    static int ChiselEdgeAt(int dir, const glm::vec3& p) {
        const glm::vec3 q = CellToShape(dir, p);
        const bool highX = q.x >= 0.5f, highZ = q.z >= 0.5f;
        return highX ? (highZ ? 3 : 1) : (highZ ? 0 : 2);
    }
    // That quarter's column, as line segments in the cell (0..1): from its bottom to its
    // top in the shape's frame (a full block: right through), turned into place.
    std::vector<glm::vec3> QuarterOutline(int dir, int edge) const {
        const float x0 = edge == 1 || edge == 3 ? 0.5f : 0.0f, z0 = edge == 0 || edge == 3 ? 0.5f : 0.0f;
        float y0 = 0.0f, y1 = 1.0f;
        constexpr float kStep = 1.0f / EdgeData::kSteps;
        if (!IsFullBlock()) y0 = edgeData.GetBottomY(edge) * kStep, y1 = std::max(edgeData.GetTopY(edge) * kStep, y0 + 0.02f);
        const glm::vec3 lo(x0, y0, z0), hi(x0 + 0.5f, y1, z0 + 0.5f);
        std::vector<glm::vec3> lines;
        auto corner = [&](int i) { return ShapeToCell(dir, glm::vec3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z)); };
        for (int i = 0; i < 8; i++)
            for (int bit : { 1, 2, 4 })
                if (!(i & bit)) lines.push_back(corner(i)), lines.push_back(corner(i | bit));
        return lines;
    }
    // The box (in the cell, 0..1) a shape fills, turned (round it: ShapeOverlaps is exact).
    void ShapeBounds(glm::vec3& lo, glm::vec3& hi) const {
        int top = 0, bottom = EdgeData::kSteps;
        for (int e = 0; e < 4; e++)
            if (!edgeData.IsEmpty(e)) top = std::max<int>(top, edgeData.GetTopY(e)), bottom = std::min<int>(bottom, edgeData.GetBottomY(e));
        if (top <= bottom) bottom = top = 0;
        constexpr float kStep = 1.0f / EdgeData::kSteps;
        const glm::vec3 a = ShapeToCell(ShapeDir(), { 0.0f, bottom * kStep, 0.0f }), b = ShapeToCell(ShapeDir(), { 1.0f, top * kStep, 1.0f });
        lo = glm::min(a, b);
        hi = glm::max(a, b);
    }
    // Chiselled (see Chiselled), from the side `dir` says when it's still a full block that
    // can turn (that side then becomes its top); a shaped one keeps its own direction.
    // How far a chisel stroke moves a corner, in sixteenths, finest first (V picks one):
    // a pixel, an eighth, a quarter, a half.
    static constexpr int kChiselSteps[] = { 1, 2, 4, 8 };
    static constexpr bool IsChiselStep(int s) { return s == 1 || s == 2 || s == 4 || s == 8; }
    // `bottom`: a stroke at the shape's underside (the side opposite its top), which on a
    // built block works its quarter's bottom instead: carving raises it, building lowers
    // it. (A full block turns to face the stroke instead; natural ground is only ever
    // worked from its top.)
    Block ChiselledFrom(int dir, int edge, int deltaSteps, bool bottom = false) const {
        Block b = *this;
        if (b.IsFullBlock() && CanTurnShape(b.type) && dir >= 0 && dir <= 5) {
            b.SetShapeDir(dir);
            return b.Chiselled(edge, deltaSteps);
        }
        return bottom && b.CanCarveBottom() ? b.ChiselledBottom(edge, deltaSteps) : b.Chiselled(edge, deltaSteps);
    }
    // Whether a shaped block's quarters can be cut from below as well (built blocks: their
    // quarter-columns have bottoms; a natural slope has only its surface).
    bool CanCarveBottom() const { return CanChisel() && IsStepped() && !IsFullBlock(); }
    // Water in a partial block sits above its slope. For flow it's treated as a cell
    // with a raised floor at the slope's average height, so the capacity is right.
    uint8_t WaterFloor() const;
    uint8_t WaterCapacity() const { return static_cast<uint8_t>(kMaxWaterVolume - WaterFloor()); }
    // Water in a partial block fills its lowest parts first. The cell is treated as
    // four columns: a stepped block's quarters, or a slope's corners (weighted like
    // the two triangles its surface is drawn as, so they hold what WaterCapacity
    // says). A notch cut into one quarter takes water before the rest of the block is
    // covered. WaterVolumeAt: the water that fits below `level` (units, 0..128 in the
    // cell); WaterLevelFor: the surface `volume` reaches (the lowest floor when dry).
    int WaterVolumeAt(int level) const;
    int WaterLevelFor(int volume) const;
    // Height of the water surface within the cell, in units.
    int WaterLevel() const { return WaterLevelFor(water); }
    // Level of the water as drawn, in units.
    int WaterShownLevel() const { return WaterLevelFor(shown); }
    // Height of the water surface as drawn, in 32nds of a block (when nothing is above
    // it): the shown level rounded. On dry ground at least an eighth, so a thin film's
    // small changes don't count as visible; on top of other water (`onWater`, the top
    // layer of a lake or pool) down to a 32nd, so a draining surface goes down evenly.
    uint8_t WaterSurface32nds(bool onWater = false) const {
        constexpr int unitsPer32nd = kMaxWaterVolume / 32;
        const int h = (WaterShownLevel() + unitsPer32nd / 2) / unitsPer32nd;
        const int minimum = onWater ? 1 : 4;
        return static_cast<uint8_t>(h < minimum ? minimum : h);
    }
    // This cell holding `volume` units: the same partial block waterlogged, or a water
    // block (air if the volume is 0). Water washes plants away.
    Block WithWater(uint8_t volume) const {
        if (!IsPartial() && volume == 0) return Block();
        Block b = IsPartial() || IsLiquid() ? *this : Water(volume);
        b.water = volume;
        // Newly wet or dried cells, and thin water, show their volume at once; otherwise
        // the drawn volume only moves once the real one is a whole drawn step away.
        const int drift = static_cast<int>(volume) - static_cast<int>(shown);
        if (volume == 0 || volume < kShownDeadband || !HasWater() || drift >= kShownDeadband || drift <= -kShownDeadband) b.shown = volume;
        return b;
    }
    // Same block apart from how much water it holds (whether it holds any counts).
    bool DiffersOnlyInWaterVolume(const Block& o) const {
        return type == o.type && edgeData == o.edgeData && axis == o.axis && HasWater() == o.HasWater();
    }

    void SetEdgeData(EdgeData edges);
    // Brings shape bytes read from a save or the network back into range: heights at
    // most 8, no bottom above its top, a plant's drop at most 8, and nothing on blocks
    // that have no shape. A shape that can't stand becomes a whole block.
    void RepairEdgeData();
    void Shrink();
    bool IsStepped() const { return StyleOf(type) == ShapeStyle::Stepped; }
    // Whether a block can be chiselled (shaped by quarters): solid building blocks,
    // not liquids, plants or machines.
    bool CanChisel() const;
    // Height of the quarter (or slope corner) `edge` (EdgeData order), moved by `deltaSteps` sixteenths.
    // Raising every corner to the top makes the block full again; lowering them all
    // removes it (air).
    Block Chiselled(int edge, int deltaSteps) const;
    // Its quarter `edge`'s bottom moved down by `deltaSteps` sixteenths (up, carving, for a
    // negative one): the column grows or shrinks from underneath.
    Block ChiselledBottom(int edge, int deltaSteps) const;

    bool IsAir() const { return type == BlockType::AIR; }
    // Full, opaque cube that hides any neighbouring face touching it, and collides.
    bool IsFullBlock() const { return type != BlockType::AIR && !IsPlant() && !IsLiquid() && !IsLava() && edgeData.IsFull(); }
    // Light passes through air, partial blocks, plants, leaves and water (water also
    // absorbs some; see LightPropagator).
    bool BlocksLight() const { return IsFullBlock() && !IsLeavesType(type) && type != BlockType::ICE && type != BlockType::GLASS && type != BlockType::SIGNAL_LAMP && type != BlockType::SIGNAL_LAMP_ON && !IsModelType(type) && !IsPipeType(type) && !IsValveType(type) && type != BlockType::PISTON_HEAD; }
    // A full block that hides the faces of the blocks it touches (not glass).
    bool HidesNeighbours() const { return IsFullBlock() && type != BlockType::GLASS && !IsModelType(type) && !IsPipeType(type) && !IsValveType(type) && type != BlockType::PISTON_HEAD; }
    // Air, a plant or water: nothing to stand on or bump into.
    bool IsPassable() const { return IsAir() || IsPlant() || IsLiquid() || IsLava(); }
    // Water may flow into this cell (washing away any plant).
    bool IsReplaceableByWater() const {
        return IsAir() || (IsPlant() && type != BlockType::LADDER && type != BlockType::DOOR_OPEN && type != BlockType::LEVER && type != BlockType::LEVER_ON);
    }
    // Drawn as a see-through cube shell (a ladder's rungs, an open door's frame).
    // Solid, but drawn by a model of its own (a turning wheel), not by the chunk mesh.
    bool IsModel() const { return IsModelType(type); }
    // Drawn as a thin tube joining its neighbours (still solid as a whole block).
    bool IsPipe() const { return IsPipeType(type); }
    bool IsShell() const { return type == BlockType::LADDER || type == BlockType::DOOR_OPEN; }
    // How thick a door's panel is (its boxes, in Resources/Data/blocks, match).
    static constexpr float kDoorThickness = 3.0f / 16.0f;
    // Wild growth that a block placed on it simply replaces (aiming at a tuft of grass
    // puts the block where the tuft was, not on top of it).
    bool IsReplaceablePlant() const { return type == BlockType::TALL_GRASS || type == BlockType::DEAD_BUSH; }
    uint8_t GetLightEmission() const { return ::GetLightEmission(type); }

    // Height of the block's top surface (0..1) at a point inside its footprint,
    // fx, fz in [0, 1]. Matches the two triangles the mesher draws for the top face,
    // so things standing on it sit exactly on the visible surface.
    float SurfaceHeight(float fx, float fz) const;

    // Appends a plant's crossed sprite (two double-sided diagonal quads), lit by the
    // light in its own cell.
    void AddPlantVertices(std::vector<uint32_t>& vertices, int x, int y, int z, uint8_t blockLight, uint8_t skyLight) const;
    // A box inside the cell, from `lo` to `hi` in eighths of a block, with this block's
    // textures (each face showing its part of them), lit evenly. Faces whose bit (1 <<
    // Face) is set in `skip` are left out.
    void AddBoxVertices(std::vector<uint32_t>& vertices, int x, int y, int z, const glm::ivec3& lo, const glm::ivec3& hi, unsigned skip, uint8_t blockLight,
                        uint8_t skyLight) const;
    void AddPebbleVertices(std::vector<uint32_t>& vertices, int x, int y, int z, uint8_t blockLight, uint8_t skyLight) const;

    // The edges of the block's shape (pairs of points, block-local), for the outline
    // around the block in view: a cube, a slope's outline, a stepped block's quarters,
    // or the box a plant stands in.
    // (`cell`: where it is, which places a pebbles block's stones.)
    // (`joined`: a pipe's arms, as for ModelBoxes.)
    std::vector<glm::vec3> OutlineSegments(const glm::ivec3& cell = glm::ivec3(0), unsigned joined = 0) const;

    // Appends 6 vertices (2 packed uint32 each) for the given face.
    void AddFaceVertices(std::vector<uint32_t>& vertices, Face face, int x, int y, int z, const FaceShading& shading) const;
    // The same with the corners' tops and bottoms given (sixteenths, EdgeData order) in
    // place of its own shape: water's surface, whose corners may come to nothing.
    void AddFaceVertices(std::vector<uint32_t>& vertices, Face face, int x, int y, int z, const FaceShading& shading, const uint8_t tops[4],
                         const uint8_t bottoms[4]) const;
    // A stepped partial block's faces pointing one way: the quarters' own faces, and
    // where a quarter stands above its neighbour, the exposed part of its side. Faces on
    // the block's outside are left out when `outside` is false (a solid block there).
    void AddSteppedFaces(std::vector<uint32_t>& vertices, Face face, int x, int y, int z, const FaceShading& shading, bool outside) const;

    bool operator==(const Block& o) const {
        return type == o.type && edgeData == o.edgeData && water == o.water && shown == o.shown && axis == o.axis;
    }
    bool operator!=(const Block& o) const { return !(*this == o); }
};

inline Block Block::Placed(BlockType t, int heightSteps, int faceAxis) {
    if (t == BlockType::WATER) return Water(kMaxWaterVolume);
    if (t == BlockType::PRESSURE_PLATE) return PressurePlate();
    if (t == BlockType::BED) return Bed();
    if (t == BlockType::GRAVE) return Grave();
    if (t == BlockType::DOOR || t == BlockType::DOOR_OPEN) { // (a panel, not a sprite's drop; hinged left or right)
        Block door = Block(t).Turned(faceAxis & 7);
        if (faceAxis & kDoorMirrored) door.axis = static_cast<uint8_t>(door.axis | kDoorMirrored);
        return door;
    }
    if (t == BlockType::LADDER) return Block(t).Turned(faceAxis); // (a panel, not a sprite's drop)
    if (IsLeverType(t)) { // (pointing out of the face it's put on)
        Block lever(t);
        lever.axis = LeverAxisFor(faceAxis >= 0 && faceAxis < 6 ? faceAxis : 2);
        return lever;
    }
    if (IsPlantType(t)) return Plant(t, 0);
    if (heightSteps >= EdgeData::kSteps || heightSteps <= 0 || !CanShape(t)) return Block(t).Turned(faceAxis);
    EdgeData slab;
    for (int e = 0; e < 4; e++) slab.SetTopY(e, static_cast<uint8_t>(heightSteps));
    return Block(t, slab).Turned(faceAxis);
}
