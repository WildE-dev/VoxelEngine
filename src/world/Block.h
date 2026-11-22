#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

// Per-column heights for a partial ("sloped") block. Each of the 4 vertical
// edges stores a bottom and top height in eighths of a block (0..8), packed as
// top in the high nibble and bottom in the low nibble.
struct EdgeData {
    uint8_t edges[4];

    EdgeData() { MakeFull(); }

    void MakeFull() {
        for (auto& e : edges) e = 0x80; // top = 8, bottom = 0
    }

    bool IsFull() const {
        for (auto e : edges)
            if (e != 0x80) return false;
        return true;
    }

    // A block is only valid if at most two of its edges are collapsed.
    bool IsValid() const {
        int collapsed = 0;
        for (int i = 0; i < 4; i++)
            if (GetTopY(i) == GetBottomY(i)) collapsed++;
        return collapsed <= 2;
    }

    void Shrink() {
        for (int i = 0; i < 4; i++) {
            uint8_t top = GetTopY(i);
            if (top > 0) SetTopY(i, top - 1);
        }
    }

    void SetBottomY(int edgeIndex, uint8_t value) {
        if (edgeIndex < 0 || edgeIndex >= 4 || value > 8) return;
        edges[edgeIndex] = (edges[edgeIndex] & 0xF0) | (value & 0x0F);
    }

    void SetTopY(int edgeIndex, uint8_t value) {
        if (edgeIndex < 0 || edgeIndex >= 4 || value > 8) return;
        edges[edgeIndex] = (edges[edgeIndex] & 0x0F) | ((value & 0x0F) << 4);
    }

    uint8_t GetBottomY(int edgeIndex) const {
        if (edgeIndex < 0 || edgeIndex >= 4) return 0;
        return edges[edgeIndex] & 0x0F;
    }

    uint8_t GetTopY(int edgeIndex) const {
        if (edgeIndex < 0 || edgeIndex >= 4) return 0;
        return (edges[edgeIndex] >> 4) & 0x0F;
    }

    bool operator==(const EdgeData& o) const {
        for (int i = 0; i < 4; i++)
            if (edges[i] != o.edges[i]) return false;
        return true;
    }
};

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
    // Items: held and used, never placed in the world (see IsItemType). Food (see
    // world/Hunger.h): venison from deer, raw or cooked.
    RAW_VENISON,
    COOKED_VENISON,
    // Deer hide, for armour (see world/Armour.h).
    HIDE,
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
    COAL,       // items
    IRON_INGOT,
    // Farming: seeds (from tall grass) planted on dirt or grass grow into a crop that
    // ripens into wheat (the server grows them: Server::UpdateCrops); wheat bakes into
    // bread.
    SEEDS,      // item
    CROP,       // plant: young
    RIPE_WHEAT, // plant: ready to harvest
    WHEAT,      // items
    BREAD,
    // A ladder: a see-through frame of rungs you walk into and climb (passable like a
    // plant; drawn as a cube).
    LADDER,
    // A door, two cells tall: closed it's solid; right-click opens it (both halves) into
    // an open frame you walk through (passable, drawn like a ladder's shell).
    DOOR,
    DOOR_OPEN,
    ARROW, // item: what a bow shoots
    RAW_FISH,    // items: caught with a fishing rod
    COOKED_FISH,
    // A campfire: a bright light (drawn as a sprite of logs and flames, passable),
    // smoking; right-click it with raw food in hand to cook it; standing in it burns.
    CAMPFIRE,
    // A sign on a post (a sprite, passable) with a line of text on it, written after
    // placing it (or rewritten with a right-click); the server keeps the texts.
    SIGN,
    // TNT: right-click to light it; three seconds later it explodes (the server runs it).
    TNT,
    TNT_LIT, // (flashing, about to go)
    BOAT,    // item: right-click water to get in
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
    // An item: steel, which only a smelter makes (from iron ingots and coal, see
    // world/Smelting.h), for the steel upgrade and drills.
    STEEL_INGOT,
    // Automation (air machines; see world/Power.h). An extractor (facing, like a
    // piston) takes things out of the chest in front of it and sends them out of its
    // back along item pipes to other chests. A filter pipe is an item pipe that lets
    // only one type through (kept in its `axis`: see Block::FilterType). A crafter makes
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
    // frames, machine casings, fire bricks); the engineer's hammer that forms them (an
    // item, held); and what a formed structure is made of: its core (drawn as the whole
    // structure's model) and its other parts (solid, drawn as nothing).
    STEEL_FRAME,
    MACHINE_CASING,
    FIRE_BRICK,
    HAMMER,
    MULTIBLOCK,
    MULTIBLOCK_PART,
    // A crusher (an air machine) grinds ore from the chests joined to it: an iron ore
    // into two crushed iron (each smelting to two ingots), stone into sand. Crushed
    // iron and coke (baked from coal in a coke oven: see world/Multiblocks.h) are items.
    CRUSHER,
    CRUSHED_IRON,
    COKE,
    // The Deep (world/TerrainGenerator.h): glowing fungus on the floors of its caverns,
    // magma (glowing, and it burns whoever stands on it), and gold ore in its rock,
    // which smelts to gold ingots (an item: what dwarf traders pay and take).
    GLOW_FUNGUS,
    MAGMA,
    GOLD_ORE,
    GOLD_INGOT,
    // A crossbow (an item, held): right-click shoots a bolt (an arrow) harder, faster
    // and flatter than a bow, but more slowly (Server::HandleShoot).
    CROSSBOW,
    COUNT
};
// Blocks that hold things like a chest (opened, filled, emptied, broken open).
inline bool IsStorageType(BlockType t) { return t == BlockType::CHEST || t == BlockType::MINING_POST; }

// Solid blocks drawn by a model of their own (turning wheels and shafts), and pipes
// drawn as thin tubes: neither hides what's beside it or blocks light.
inline bool IsModelType(BlockType type) {
    return type == BlockType::WATER_WHEEL || type == BlockType::SHAFT || type == BlockType::PRESSURE_GAUGE || type == BlockType::MULTIBLOCK ||
           type == BlockType::MULTIBLOCK_PART;
}
inline bool IsPipeType(BlockType type) {
    return type == BlockType::PIPE || type == BlockType::AIR_PIPE || type == BlockType::ITEM_PIPE || type == BlockType::FILTER_PIPE || type == BlockType::FLUID_PIPE;
}
// Blocks that face one of six ways (their `axis` holds the Face they point to): a
// piston and its head.
// Water goes down these and is gone (see Water.h).
inline bool IsDrainType(BlockType type) { return type == BlockType::DRAIN || type == BlockType::SINKHOLE; }
inline bool IsFacingType(BlockType type) {
    return type == BlockType::PISTON || type == BlockType::PISTON_HEAD || type == BlockType::EXTRACTOR || type == BlockType::PRESSURE_GAUGE ||
           type == BlockType::REFINERY || type == BlockType::FLUID_OUTLET;
}

// Things held in the inventory that aren't blocks: they can't be placed.
inline bool IsItemType(BlockType type) {
    return type == BlockType::RAW_VENISON || type == BlockType::COOKED_VENISON || type == BlockType::HIDE || type == BlockType::COAL ||
           type == BlockType::IRON_INGOT || type == BlockType::SEEDS || type == BlockType::WHEAT || type == BlockType::BREAD ||
           type == BlockType::ARROW || type == BlockType::RAW_FISH || type == BlockType::COOKED_FISH || type == BlockType::BOAT ||
           type == BlockType::STEEL_INGOT || type == BlockType::HAMMER || type == BlockType::CRUSHED_IRON || type == BlockType::COKE ||
           type == BlockType::GOLD_INGOT || type == BlockType::CROSSBOW;
}

inline bool IsPlantType(BlockType type) {
    return type == BlockType::TALL_GRASS || type == BlockType::FLOWER_RED || type == BlockType::FLOWER_YELLOW || type == BlockType::SPIKES ||
           type == BlockType::DEAD_BUSH || type == BlockType::PEBBLES || type == BlockType::CROP || type == BlockType::RIPE_WHEAT ||
           type == BlockType::LADDER || type == BlockType::DOOR_OPEN || type == BlockType::CAMPFIRE ||
           type == BlockType::SIGN || type == BlockType::SAPLING || type == BlockType::PINE_SAPLING || type == BlockType::LEVER ||
           type == BlockType::LEVER_ON || type == BlockType::GLOW_FUNGUS;
}

// The resource a block counts as in survival (see Block::ResourceUnits): its own type,
// except pebbles, which are stone.
// What breaking a block gives: pebbles are stone, coal ore is coal.
inline BlockType ResourceTypeOf(BlockType type) {
    switch (type) {
    case BlockType::PEBBLES: return BlockType::STONE;
    case BlockType::COAL_ORE: return BlockType::COAL;
    case BlockType::CROP: return BlockType::SEEDS;       // (an unripe crop gives its seed back; planting costs one)
    case BlockType::RIPE_WHEAT: return BlockType::WHEAT; // (and the server adds seeds)
    case BlockType::DOOR_OPEN: return BlockType::DOOR;
    case BlockType::TNT_LIT: return BlockType::TNT;
    case BlockType::LEVER_ON: return BlockType::LEVER;
    case BlockType::SIGNAL_LAMP_ON: return BlockType::SIGNAL_LAMP;
    case BlockType::VALVE_OPEN: return BlockType::VALVE;
    case BlockType::PISTON_HEAD: return BlockType::AIR; // (part of the piston: gives nothing)
    case BlockType::SMELTER_LIT: return BlockType::SMELTER;
    case BlockType::SPRING:
    case BlockType::SINKHOLE:
    case BlockType::OIL_SHALE: return BlockType::STONE;
    case BlockType::MULTIBLOCK:
    case BlockType::MULTIBLOCK_PART: return BlockType::AIR; // (taking it apart gives back what it was built of)
    default: return type;
    }
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
         : type == BlockType::GLOW_FUNGUS ? 11 : type == BlockType::MAGMA ? 12 : 0;
}
namespace BlockData { extern int8_t gLight[]; }
inline uint8_t GetLightEmission(BlockType type) {
    const int8_t set = BlockData::gLight[static_cast<size_t>(type)];
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
    // the Face it points to here (0..5) instead; a filter pipe the BlockType it lets
    // through (0, air: anything); a crafter its recipe (index + 1; 0: none). Always 0
    // for others.
    uint8_t axis = 0;
    static constexpr uint8_t kUpright = 0, kAlongX = 1, kAlongZ = 2;
    static constexpr uint8_t kMaxAxis = 5; // (a facing block's Face)
    // A filter pipe's type (AIR: it lets anything through).
    BlockType FilterType() const { return axis < static_cast<uint8_t>(BlockType::COUNT) ? static_cast<BlockType>(axis) : BlockType::AIR; }
    // A facing block's direction (a unit step).
    glm::ivec3 Facing() const { return glm::ivec3(kFaceOffsets[axis % 6][0], kFaceOffsets[axis % 6][1], kFaceOffsets[axis % 6][2]); }
    static bool IsOrientable(BlockType t) { return t == BlockType::LOG || t == BlockType::WATER_WHEEL || t == BlockType::SHAFT; }
    // The axis in kFaceAxis terms (0 x, 1 y, 2 z), and the orientation for one.
    int GrainAxis() const { return axis == kAlongX ? 0 : axis == kAlongZ ? 2 : 1; }
    static uint8_t AxisFor(int faceAxis) { return faceAxis == 0 ? kAlongX : faceAxis == 2 ? kAlongZ : kUpright; }
    // The same block turned to run along `faceAxis` (if it can be). A facing block
    // takes a Face (0..5) instead.
    Block Turned(int faceAxis) const {
        Block b = *this;
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

    // A plant whose sprite is drawn `dropEighths` (0..8) below its cell's floor, so it
    // stands on a sloped block underneath. Stored in the otherwise unused edge data.
    // A pressure plate: a slab an eighth of a block high.
    static Block Bed() {
        EdgeData edges;
        for (int e = 0; e < 4; e++) edges.SetTopY(e, 4); // half a block high
        return Block(BlockType::BED, edges);
    }
    static Block PressurePlate() {
        EdgeData edges;
        for (int e = 0; e < 4; e++) edges.SetTopY(e, 1);
        return Block(BlockType::PRESSURE_PLATE, edges);
    }
    // What placing a block of this type puts in the world (a full cell of water, a
    // plate's slab, a sprite standing on the floor below). Blocks that can be shaped
    // may be placed lower than full: a slab `heightEighths` high (2, 4 or 6 of 8).
    // Orientable blocks run along `faceAxis` (0 x, 1 y, 2 z): the axis of the face
    // they were placed against.
    static Block Placed(BlockType t, int heightEighths = 8, int faceAxis = 1);
    // Whether a type can be placed as a slab (and chiselled): building blocks, not
    // liquids, plants or machines.
    static bool CanShape(BlockType t) { return Block(t).CanChisel(); }

    // Resources (survival mode): a full block is worth kUnitsPerBlock units of its
    // type; a shaped one is worth its volume (a half slab 32, each eighth of a
    // quarter 2). Things that can't be shaped (plants, machines) are worth a full block.
    static constexpr int kUnitsPerBlock = 64;
    static constexpr int kPebbleUnits = 16; // a quarter of a block of stone
    int ResourceUnits() const;
    BlockType ResourceType() const { return ResourceTypeOf(type); }

    static Block Plant(BlockType t, uint8_t dropEighths) {
        Block b(t);
        b.edgeData.edges[0] = dropEighths;
        return b;
    }
    bool IsPlant() const { return IsPlantType(type); }
    float PlantDrop() const { return (edgeData.edges[0] & 0x0F) / 8.0f; }

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
    bool HasWater() const { return water > 0; }
    uint8_t WaterVolume() const { return water; }
    bool IsFullWater() const { return IsLiquid() && water == kMaxWaterVolume; }
    // A sloped block: solid below its surface, open above it.
    bool IsPartial() const { return !IsAir() && !IsPlant() && !IsLiquid() && !IsFullBlock(); }
    bool CanHoldWater() const { return IsReplaceableByWater() || IsLiquid() || IsPartial(); }
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
    void Shrink();
    bool IsStepped() const { return StyleOf(type) == ShapeStyle::Stepped; }
    // Whether a block can be chiselled (shaped by quarters): solid building blocks,
    // not liquids, plants or machines.
    bool CanChisel() const;
    // Height of the quarter (or slope corner) `edge` (EdgeData order), in eighths.
    // Raising every corner to the top makes the block full again; lowering them all
    // removes it (air).
    Block Chiselled(int edge, int deltaEighths) const;

    bool IsAir() const { return type == BlockType::AIR; }
    // Full, opaque cube that hides any neighbouring face touching it, and collides.
    bool IsFullBlock() const { return type != BlockType::AIR && !IsPlant() && !IsLiquid() && edgeData.IsFull(); }
    // Light passes through air, partial blocks, plants, leaves and water (water also
    // absorbs some; see LightPropagator).
    bool BlocksLight() const { return IsFullBlock() && !IsLeavesType(type) && type != BlockType::ICE && type != BlockType::GLASS && type != BlockType::SIGNAL_LAMP && type != BlockType::SIGNAL_LAMP_ON && !IsModelType(type) && !IsPipeType(type) && type != BlockType::PISTON_HEAD; }
    // A full block that hides the faces of the blocks it touches (not glass).
    bool HidesNeighbours() const { return IsFullBlock() && type != BlockType::GLASS && !IsModelType(type) && !IsPipeType(type) && type != BlockType::PISTON_HEAD; }
    // Air, a plant or water: nothing to stand on or bump into.
    bool IsPassable() const { return IsAir() || IsPlant() || IsLiquid(); }
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
    std::vector<glm::vec3> OutlineSegments() const;

    // Appends 6 vertices (2 packed uint32 each) for the given face.
    void AddFaceVertices(std::vector<uint32_t>& vertices, Face face, int x, int y, int z, const FaceShading& shading) const;
    // A stepped partial block's faces pointing one way: the quarters' own faces, and
    // where a quarter stands above its neighbour, the exposed part of its side. Faces on
    // the block's outside are left out when `outside` is false (a solid block there).
    void AddSteppedFaces(std::vector<uint32_t>& vertices, Face face, int x, int y, int z, const FaceShading& shading, bool outside) const;

    bool operator==(const Block& o) const {
        return type == o.type && edgeData == o.edgeData && water == o.water && shown == o.shown && axis == o.axis;
    }
    bool operator!=(const Block& o) const { return !(*this == o); }
};

inline Block Block::Placed(BlockType t, int heightEighths, int faceAxis) {
    if (t == BlockType::WATER) return Water(kMaxWaterVolume);
    if (t == BlockType::PRESSURE_PLATE) return PressurePlate();
    if (t == BlockType::BED) return Bed();
    if (t == BlockType::LADDER || t == BlockType::DOOR_OPEN) return Block(t); // (a whole cell, not a sprite's drop)
    if (IsPlantType(t)) return Plant(t, 0);
    if (heightEighths >= 8 || heightEighths <= 0 || !CanShape(t)) return Block(t).Turned(faceAxis);
    EdgeData slab;
    for (int e = 0; e < 4; e++) slab.SetTopY(e, static_cast<uint8_t>(heightEighths));
    return Block(t, slab).Turned(faceAxis);
}
