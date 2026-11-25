#include "world/Block.h"
#include "world/BlockData.h"
#include "world/Coords.h"
#include "world/SpriteMasks.h"

#include <algorithm>
#include <array>
#include <atomic>

namespace {

// Unpacked vertex, packed into two uint32s for upload. See main.vert for the decode side.
struct VertexData {
    glm::vec3 pos{0.0f};
    glm::vec2 tex{0.0f};
    int textureIndex = 0;
    glm::vec3 norm{0.0f};
    uint32_t ao = 3;
    uint32_t light = 0;
    uint32_t sky = 0;

    VertexData() = default;
    VertexData(glm::vec3 pos, glm::vec2 tex, int textureIndex, glm::vec3 norm)
        : pos(pos), tex(tex), textureIndex(textureIndex), norm(norm) {}

    // xyz in 1/8 block units (8 bits each), uv in 1/8 units (4 bits each). y is
    // stored one block up so plant sprites can reach below their chunk's floor.
    uint32_t PackPosition() const {
        uint32_t x = static_cast<uint32_t>(pos.x * 8);
        uint32_t y = static_cast<uint32_t>((pos.y + 1.0f) * 8);
        uint32_t z = static_cast<uint32_t>(pos.z * 8);
        uint32_t u = static_cast<uint32_t>(tex.x * 8);
        uint32_t v = static_cast<uint32_t>(tex.y * 8);
        return ((x & 0xFFu) << 24) | ((y & 0xFFu) << 16) | ((z & 0xFFu) << 8) | ((u & 0xFu) << 4) | (v & 0xFu);
    }

    // Normal quantised to 4 bits per axis, 2-bit ambient occlusion, 4-bit block
    // light, 4-bit sky light, 10-bit texture index.
    uint32_t PackShading() const {
        uint32_t nx = static_cast<uint32_t>((norm.x + 1.0f) * 7.5f);
        uint32_t ny = static_cast<uint32_t>((norm.y + 1.0f) * 7.5f);
        uint32_t nz = static_cast<uint32_t>((norm.z + 1.0f) * 7.5f);
        return ((nx & 0xFu) << 28) | ((ny & 0xFu) << 24) | ((nz & 0xFu) << 20) |
               ((ao & 0x3u) << 18) | ((light & 0xFu) << 14) | ((sky & 0xFu) << 10) |
               (static_cast<uint32_t>(textureIndex) & 0x3FFu);
    }
};

glm::vec3 GetNormal(glm::vec3 p1, glm::vec3 p2, glm::vec3 p3) {
    glm::vec3 A = p3 - p1;
    glm::vec3 B = p2 - p1;
    glm::vec3 cross = glm::normalize(glm::cross(A, B));
    return cross;
}

std::array<VertexData, 6> GetFaceVertices(Face face, const EdgeData& edgeData, int x, int y, int z, const TextureData& textureData) {
    std::array<VertexData, 6> vertices;

    float bottomY0 = static_cast<float>(edgeData.GetBottomY(0)) / 8.0f;
    float bottomY1 = static_cast<float>(edgeData.GetBottomY(1)) / 8.0f;
    float bottomY2 = static_cast<float>(edgeData.GetBottomY(2)) / 8.0f;
    float bottomY3 = static_cast<float>(edgeData.GetBottomY(3)) / 8.0f;

    float topY0 = static_cast<float>(edgeData.GetTopY(0)) / 8.0f;
    float topY1 = static_cast<float>(edgeData.GetTopY(1)) / 8.0f;
    float topY2 = static_cast<float>(edgeData.GetTopY(2)) / 8.0f;
    float topY3 = static_cast<float>(edgeData.GetTopY(3)) / 8.0f;

    switch (face) {
    case Face::Right:
    {
        glm::vec3 pos1 = glm::vec3(x + 1.0f, y + bottomY1, z);
        glm::vec3 pos2 = glm::vec3(x + 1.0f, y + bottomY3, z + 1.0f);
        glm::vec3 pos3 = glm::vec3(x + 1.0f, y + topY1, z);
        glm::vec3 pos4 = glm::vec3(x + 1.0f, y + topY3, z + 1.0f);

        glm::vec2 tex1 = glm::vec2(1.0f, topY1);
        glm::vec2 tex2 = glm::vec2(0.0f, topY3);
        glm::vec2 tex3 = glm::vec2(1.0f, bottomY1);
        glm::vec2 tex4 = glm::vec2(0.0f, bottomY3);

        glm::vec3 norm1 = GetNormal(pos1, pos2, pos3);
        glm::vec3 norm2 = GetNormal(pos3, pos2, pos4);

        vertices[0] = VertexData(pos1, tex1, textureData.right, norm1);
        vertices[1] = VertexData(pos2, tex2, textureData.right, norm1);
        vertices[2] = VertexData(pos3, tex3, textureData.right, norm1);

        vertices[3] = VertexData(pos3, tex3, textureData.right, norm2);
        vertices[4] = VertexData(pos2, tex2, textureData.right, norm2);
        vertices[5] = VertexData(pos4, tex4, textureData.right, norm2);
        break;
    }
    case Face::Left:
    {
        glm::vec3 pos1 = glm::vec3(x, y + bottomY0, z + 1.0f);
        glm::vec3 pos2 = glm::vec3(x, y + bottomY2, z);
        glm::vec3 pos3 = glm::vec3(x, y + topY0, z + 1.0f);
        glm::vec3 pos4 = glm::vec3(x, y + topY2, z);

        glm::vec2 tex1 = glm::vec2(1.0f, topY0);
        glm::vec2 tex2 = glm::vec2(0.0f, topY2);
        glm::vec2 tex3 = glm::vec2(1.0f, bottomY0);
        glm::vec2 tex4 = glm::vec2(0.0f, bottomY2);

        glm::vec3 norm1 = GetNormal(pos1, pos2, pos3);
        glm::vec3 norm2 = GetNormal(pos3, pos2, pos4);

        vertices[0] = VertexData(pos1, tex1, textureData.left, norm1);
        vertices[1] = VertexData(pos2, tex2, textureData.left, norm1);
        vertices[2] = VertexData(pos3, tex3, textureData.left, norm1);

        vertices[3] = VertexData(pos3, tex3, textureData.left, norm2);
        vertices[4] = VertexData(pos2, tex2, textureData.left, norm2);
        vertices[5] = VertexData(pos4, tex4, textureData.left, norm2);
        break;
    }
    case Face::Top:
    {
        glm::vec3 pos1 = glm::vec3(x + 1.0f, y + topY3, z + 1.0f);
        glm::vec3 pos2 = glm::vec3(x, y + topY0, z + 1.0f);
        glm::vec3 pos3 = glm::vec3(x + 1.0f, y + topY1, z);
        glm::vec3 pos4 = glm::vec3(x, y + topY2, z);

        glm::vec2 tex1 = glm::vec2(1.0f, 1.0f);
        glm::vec2 tex2 = glm::vec2(0.0f, 1.0f);
        glm::vec2 tex3 = glm::vec2(1.0f, 0.0f);
        glm::vec2 tex4 = glm::vec2(0.0f, 0.0f);

        glm::vec3 norm1 = GetNormal(pos1, pos2, pos3);
        glm::vec3 norm2 = GetNormal(pos3, pos2, pos4);

        vertices[0] = VertexData(pos1, tex1, textureData.top, norm1);
        vertices[1] = VertexData(pos2, tex2, textureData.top, norm1);
        vertices[2] = VertexData(pos3, tex3, textureData.top, norm1);

        vertices[3] = VertexData(pos3, tex3, textureData.top, norm2);
        vertices[4] = VertexData(pos2, tex2, textureData.top, norm2);
        vertices[5] = VertexData(pos4, tex4, textureData.top, norm2);
        break;
    }
    case Face::Bottom:
    {
        glm::vec3 pos1 = glm::vec3(x, y + bottomY2, z);
        glm::vec3 pos2 = glm::vec3(x, y + bottomY0, z + 1.0f);
        glm::vec3 pos3 = glm::vec3(x + 1.0f, y + bottomY1, z);
        glm::vec3 pos4 = glm::vec3(x + 1.0f, y + bottomY3, z + 1.0f);

        glm::vec2 tex1 = glm::vec2(1.0f, 1.0f);
        glm::vec2 tex2 = glm::vec2(0.0f, 1.0f);
        glm::vec2 tex3 = glm::vec2(1.0f, 0.0f);
        glm::vec2 tex4 = glm::vec2(0.0f, 0.0f);

        glm::vec3 norm1 = GetNormal(pos1, pos2, pos3);
        glm::vec3 norm2 = GetNormal(pos3, pos2, pos4);

        vertices[0] = VertexData(pos1, tex1, textureData.bottom, norm1);
        vertices[1] = VertexData(pos2, tex2, textureData.bottom, norm1);
        vertices[2] = VertexData(pos3, tex3, textureData.bottom, norm1);

        vertices[3] = VertexData(pos3, tex3, textureData.bottom, norm2);
        vertices[4] = VertexData(pos2, tex2, textureData.bottom, norm2);
        vertices[5] = VertexData(pos4, tex4, textureData.bottom, norm2);
        break;
    }
    case Face::Back:
    {
        glm::vec3 pos1 = glm::vec3(x, y + bottomY0, z + 1.0f);
        glm::vec3 pos2 = glm::vec3(x, y + topY0, z + 1.0f);
        glm::vec3 pos3 = glm::vec3(x + 1.0f, y + bottomY3, z + 1.0f);
        glm::vec3 pos4 = glm::vec3(x + 1.0f, y + topY3, z + 1.0f);

        glm::vec2 tex1 = glm::vec2(0.0f, topY0);
        glm::vec2 tex2 = glm::vec2(0.0f, bottomY0);
        glm::vec2 tex3 = glm::vec2(1.0f, topY3);
        glm::vec2 tex4 = glm::vec2(1.0f, bottomY3);

        glm::vec3 norm1 = GetNormal(pos1, pos2, pos3);
        glm::vec3 norm2 = GetNormal(pos3, pos2, pos4);

        vertices[0] = VertexData(pos1, tex1, textureData.back, norm1);
        vertices[1] = VertexData(pos2, tex2, textureData.back, norm1);
        vertices[2] = VertexData(pos3, tex3, textureData.back, norm1);

        vertices[3] = VertexData(pos3, tex3, textureData.back, norm2);
        vertices[4] = VertexData(pos2, tex2, textureData.back, norm2);
        vertices[5] = VertexData(pos4, tex4, textureData.back, norm2);
        break;
    }
    case Face::Front:
    {
        glm::vec3 pos1 = glm::vec3(x + 1.0f, y + bottomY1, z);
        glm::vec3 pos2 = glm::vec3(x + 1.0f, y + topY1, z);
        glm::vec3 pos3 = glm::vec3(x, y + bottomY2, z);
        glm::vec3 pos4 = glm::vec3(x, y + topY2, z);

        glm::vec2 tex1 = glm::vec2(0.0f, topY1);
        glm::vec2 tex2 = glm::vec2(0.0f, bottomY1);
        glm::vec2 tex3 = glm::vec2(1.0f, topY2);
        glm::vec2 tex4 = glm::vec2(1.0f, bottomY2);

        glm::vec3 norm1 = GetNormal(pos1, pos2, pos3);
        glm::vec3 norm2 = GetNormal(pos3, pos2, pos4);

        vertices[0] = VertexData(pos1, tex1, textureData.front, norm1);
        vertices[1] = VertexData(pos2, tex2, textureData.front, norm1);
        vertices[2] = VertexData(pos3, tex3, textureData.front, norm1);

        vertices[3] = VertexData(pos3, tex3, textureData.front, norm2);
        vertices[4] = VertexData(pos2, tex2, textureData.front, norm2);
        vertices[5] = VertexData(pos4, tex4, textureData.front, norm2);
        break;
    }
    }

    return vertices;
}

} // namespace

namespace {

// Texture file names (without extension) in Resources/Textures/Blocks, per face:
// top, bottom, front, back, left, right.
struct BlockTextureNames {
    const char* faces[6];
};

constexpr int kBlockTypeCount = static_cast<int>(BlockType::COUNT);

const BlockTextureNames kBlockTextureNames[kBlockTypeCount] = {
    /* AIR   */ {{ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* GRASS */ {{ "grass_top", "dirt", "grass_side", "grass_side", "grass_side", "grass_side" }},
    /* DIRT  */ {{ "dirt", "dirt", "dirt", "dirt", "dirt", "dirt" }},
    /* STONE */ {{ "stone", "stone", "stone", "stone", "stone", "stone" }},
    /* LAMP  */ {{ "lamp", "lamp", "lamp", "lamp", "lamp", "lamp" }},
    /* LOG   */ {{ "log_top", "log_top", "log_side", "log_side", "log_side", "log_side" }},
    /* LEAVES*/ {{ "leaves", "leaves", "leaves", "leaves", "leaves", "leaves" }},
    /* TALL_GRASS    */ {{ "tall_grass", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* FLOWER_RED    */ {{ "flower_red", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* FLOWER_YELLOW */ {{ "flower_yellow", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* SAND  */ {{ "sand", "sand", "sand", "sand", "sand", "sand" }},
    /* WATER */ {{ "water", "water", "water", "water", "water", "water" }},
    /* PUMP  */ {{ "pump_top", "pump_top", "pump_side", "pump_side", "pump_side", "pump_side" }},
    /* PIPE  */ {{ "pipe_end", "pipe_end", "pipe_end", "pipe_end", "pipe_end", "pipe_end" }},
    /* STONE_BRICK */ {{ "stone_brick", "stone_brick", "stone_brick", "stone_brick", "stone_brick", "stone_brick" }},
    /* MOSSY_BRICK */ {{ "mossy_brick", "mossy_brick", "mossy_brick", "mossy_brick", "mossy_brick", "mossy_brick" }},
    /* SPIKES */ {{ "spikes", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* CRUMBLING_BRICK */ {{ "cracked_brick", "cracked_brick", "cracked_brick", "cracked_brick", "cracked_brick", "cracked_brick" }},
    /* PRESSURE_PLATE */ {{ "pressure_plate", "pressure_plate", "pressure_plate", "pressure_plate", "pressure_plate", "pressure_plate" }},
    /* SLUICE */ {{ "sluice", "sluice", "sluice", "sluice", "sluice", "sluice" }},
    /* SNOW  */ {{ "snow", "stone", "snow_side", "snow_side", "snow_side", "snow_side" }},
    /* DRY_GRASS   */ {{ "dry_grass_top", "dirt", "dry_grass_side", "dry_grass_side", "dry_grass_side", "dry_grass_side" }},
    /* LUSH_GRASS  */ {{ "lush_grass_top", "dirt", "lush_grass_side", "lush_grass_side", "lush_grass_side", "lush_grass_side" }},
    /* SNOWY_GRASS */ {{ "snowy_grass_top", "dirt", "snowy_grass_side", "snowy_grass_side", "snowy_grass_side", "snowy_grass_side" }},
    /* PINE_LEAVES */ {{ "pine_leaves", "pine_leaves", "pine_leaves", "pine_leaves", "pine_leaves", "pine_leaves" }},
    /* CACTUS */ {{ "cactus_top", "cactus_top", "cactus_side", "cactus_side", "cactus_side", "cactus_side" }},
    /* DEAD_BUSH */ {{ "dead_bush", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* ICE   */ {{ "ice", "ice", "ice", "ice", "ice", "ice" }},
    /* PEBBLES */ {{ "pebbles", "stone", nullptr, nullptr, nullptr, nullptr }}, // icon; the stones' own texture
    /* PLANKS */ {{ "planks", "planks", "planks", "planks", "planks", "planks" }},
    /* GLASS  */ {{ "glass", "glass", "glass", "glass", "glass", "glass" }},
    /* CHEST */ {{ "chest_top", "chest_top", "chest_side", "chest_side", "chest_side", "chest_side" }},
    /* BED   */ {{ "bed_top", "planks", "bed_side", "bed_side", "bed_side", "bed_side" }},
    /* COAL_ORE   */ {{ "coal_ore", "coal_ore", "coal_ore", "coal_ore", "coal_ore", "coal_ore" }},
    /* IRON_ORE   */ {{ "iron_ore", "iron_ore", "iron_ore", "iron_ore", "iron_ore", "iron_ore" }},
    /* CROP       */ {{ "crop", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* RIPE_WHEAT */ {{ "ripe_wheat", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* LADDER     */ {{ "ladder", "ladder", "ladder", "ladder", "ladder", "ladder" }},
    /* DOOR       */ {{ "planks", "planks", "door", "door", "door", "door" }},
    /* DOOR_OPEN  */ {{ "door_open", "door_open", "door_open", "door_open", "door_open", "door_open" }},
    /* CAMPFIRE    */ {{ "campfire", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* SIGN        */ {{ "sign", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* TNT         */ {{ "tnt_top", "tnt_top", "tnt_side", "tnt_side", "tnt_side", "tnt_side" }},
    /* TNT_LIT     */ {{ "tnt_lit", "tnt_lit", "tnt_lit", "tnt_lit", "tnt_lit", "tnt_lit" }},
    /* SAPLING     */ {{ "sapling", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* PINE_SAPLING */ {{ "pine_sapling", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* WATER_WHEEL */ {{ "water_wheel", "water_wheel", "water_wheel", "water_wheel", "water_wheel", "water_wheel" }}, // icon (drawn as a model)
    /* AIR_PIPE    */ {{ "air_pipe_end", "air_pipe_end", "air_pipe", "air_pipe", "air_pipe", "air_pipe" }},
    /* DRILL       */ {{ "drill_top", "drill_bottom", "drill_side", "drill_side", "drill_side", "drill_side" }},
    /* SHAFT       */ {{ "shaft", "shaft", "shaft", "shaft", "shaft", "shaft" }}, // icon (drawn as a model)
    /* COMPRESSOR  */ {{ "compressor_top", "compressor_top", "compressor_side", "compressor_side", "compressor_side", "compressor_side" }},
    /* LEVER       */ {{ "lever", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* LEVER_ON    */ {{ "lever_on", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* LOGIC_GATE  */ {{ "logic_gate_top", "logic_gate_side", "logic_gate_side", "logic_gate_side", "logic_gate_side", "logic_gate_side" }},
    /* SIGNAL_LAMP */ {{ "signal_lamp", "signal_lamp", "signal_lamp", "signal_lamp", "signal_lamp", "signal_lamp" }},
    /* SIGNAL_LAMP_ON */ {{ "signal_lamp_on", "signal_lamp_on", "signal_lamp_on", "signal_lamp_on", "signal_lamp_on", "signal_lamp_on" }},
    /* AIR_TANK    */ {{ "air_tank_top", "air_tank_top", "air_tank_side", "air_tank_side", "air_tank_side", "air_tank_side" }},
    /* VALVE       */ {{ "valve_handle", "valve_handle", "valve_icon", "valve_icon", "valve_icon", "valve_icon" }},
    /* VALVE_OPEN  */ {{ "valve_handle", "valve_handle", "valve_icon", "valve_icon", "valve_icon", "valve_icon" }},
    // (Facing blocks: "top" is the face they point to, "bottom" the back, "front" the sides.)
    /* PISTON      */ {{ "piston_face", "piston_back", "piston_side", "piston_side", "piston_side", "piston_side" }},
    /* PISTON_HEAD */ {{ "piston_face", "piston_face", "piston_rod", "piston_rod", "piston_rod", "piston_rod" }},
    /* WATER_SENSOR */ {{ "water_sensor", "sensor_bottom", "water_sensor", "water_sensor", "water_sensor", "water_sensor" }},
    /* PRESSURE_SENSOR */ {{ "pressure_sensor", "sensor_bottom", "pressure_sensor", "pressure_sensor", "pressure_sensor", "pressure_sensor" }},
    /* DAYLIGHT_SENSOR */ {{ "daylight_sensor", "sensor_bottom", "sensor_side", "sensor_side", "sensor_side", "sensor_side" }},
    /* PLAYER_SENSOR */ {{ "player_sensor", "sensor_bottom", "player_sensor", "player_sensor", "player_sensor", "player_sensor" }},
    /* ITEM_PIPE   */ {{ "item_pipe_end", "item_pipe_end", "item_pipe", "item_pipe", "item_pipe", "item_pipe" }},
    /* SMELTER     */ {{ "smelter_top", "smelter_top", "smelter_front", "smelter_side", "smelter_side", "smelter_side" }},
    /* SMELTER_LIT */ {{ "smelter_top", "smelter_top", "smelter_front_lit", "smelter_side", "smelter_side", "smelter_side" }},
    /* SPRING      */ {{ "spring", "spring", "spring", "spring", "spring", "spring" }},
    /* EXTRACTOR   */ {{ "extractor_intake", "extractor_out", "extractor_side", "extractor_side", "extractor_side", "extractor_side" }},
    /* FILTER_PIPE */ {{ "filter_pipe_end", "filter_pipe_end", "filter_pipe", "filter_pipe", "filter_pipe", "filter_pipe" }},
    /* CRAFTER     */ {{ "crafter_top", "crafter_bottom", "crafter_side", "crafter_side", "crafter_side", "crafter_side" }},
    /* HARVESTER   */ {{ "harvester_top", "crafter_bottom", "harvester_side", "harvester_side", "harvester_side", "harvester_side" }},
    /* DRAIN       */ {{ "drain", "drain_side", "drain_side", "drain_side", "drain_side", "drain_side" }},
    /* SINKHOLE    */ {{ "sinkhole", "sinkhole_side", "sinkhole_side", "sinkhole_side", "sinkhole_side", "sinkhole_side" }},
    /* PRESSURE_GAUGE */ {{ "pressure_gauge", "pressure_gauge", "pressure_gauge", "pressure_gauge", "pressure_gauge", "pressure_gauge" }}, // icon (drawn as a model)
    /* OIL_SHALE   */ {{ "oil_shale", "oil_shale", "oil_shale", "oil_shale", "oil_shale", "oil_shale" }},
    /* TAR         */ {{ "tar", "dirt", "tar_side", "tar_side", "tar_side", "tar_side" }},
    /* FLUID_PIPE  */ {{ "fluid_pipe_end", "fluid_pipe_end", "fluid_pipe", "fluid_pipe", "fluid_pipe", "fluid_pipe" }},
    /* FLUID_TANK  */ {{ "fluid_tank_top", "fluid_tank_top", "fluid_tank_side", "fluid_tank_side", "fluid_tank_side", "fluid_tank_side" }},
    /* OIL_DERRICK */ {{ "derrick_top", "derrick_bottom", "derrick_side", "derrick_side", "derrick_side", "derrick_side" }},
    /* REFINERY    */ {{ "refinery_out", "refinery_in", "refinery_side", "refinery_side", "refinery_side", "refinery_side" }},
    /* ENGINE      */ {{ "engine_top", "engine_top", "engine_side", "engine_side", "engine_side", "engine_side" }},
    /* FLUID_OUTLET */ {{ "outlet_face", "outlet_back", "outlet_side", "outlet_side", "outlet_side", "outlet_side" }},
    /* MINING_POST */ {{ "mining_post_top", "planks", "mining_post_side", "mining_post_side", "mining_post_side", "mining_post_side" }},
    /* STEEL_FRAME */ {{ "steel_frame", "steel_frame", "steel_frame", "steel_frame", "steel_frame", "steel_frame" }},
    /* MACHINE_CASING */ {{ "machine_casing", "machine_casing", "machine_casing", "machine_casing", "machine_casing", "machine_casing" }},
    /* FIRE_BRICK  */ {{ "fire_brick", "fire_brick", "fire_brick", "fire_brick", "fire_brick", "fire_brick" }},
    /* MULTIBLOCK  */ {{ "machine_casing", "machine_casing", "machine_casing", "machine_casing", "machine_casing", "machine_casing" }}, // (drawn as a model)
    /* MULTIBLOCK_PART */ {{ "machine_casing", "machine_casing", "machine_casing", "machine_casing", "machine_casing", "machine_casing" }},
    /* CRUSHER     */ {{ "crusher_top", "machine_casing", "crusher_side", "crusher_side", "crusher_side", "crusher_side" }},
    /* GLOW_FUNGUS */ {{ "glow_fungus", "glow_fungus", "glow_fungus", "glow_fungus", "glow_fungus", "glow_fungus" }},
    /* MAGMA       */ {{ "magma", "magma", "magma", "magma", "magma", "magma" }},
    /* GOLD_ORE    */ {{ "gold_ore", "gold_ore", "gold_ore", "gold_ore", "gold_ore", "gold_ore" }},
    /* MACHINE_SENSOR */ {{ "machine_sensor", "sensor_bottom", "machine_sensor", "machine_sensor", "machine_sensor", "machine_sensor" }},
    /* BOILER      */ {{ "boiler_top", "boiler_top", "boiler_side", "boiler_side", "boiler_side", "boiler_side" }},
    /* TURBINE     */ {{ "turbine_top", "turbine_top", "turbine_side", "turbine_side", "turbine_side", "turbine_side" }},
    /* AIR_DIFFUSER */ {{ "air_diffuser_top", "machine_casing", "air_diffuser_side", "air_diffuser_side", "air_diffuser_side", "air_diffuser_side" }},
    /* COPPER_ORE  */ {{ "copper_ore", "copper_ore", "copper_ore", "copper_ore", "copper_ore", "copper_ore" }},
    /* CREATIVE_AIR_TANK */ {{ "creative_air_tank_top", "creative_air_tank_top", "creative_air_tank_side", "creative_air_tank_side", "creative_air_tank_side", "creative_air_tank_side" }},
    /* LAVA        */ {{ "lava", "lava", "lava", "lava", "lava", "lava" }},
    /* FLUID_VALVE */ {{ "valve_handle", "valve_handle", "fluid_valve_icon", "fluid_valve_icon", "fluid_valve_icon", "fluid_valve_icon" }},
    /* FLUID_VALVE_OPEN */ {{ "valve_handle", "valve_handle", "fluid_valve_icon", "fluid_valve_icon", "fluid_valve_icon", "fluid_valve_icon" }},
    /* OBSIDIAN    */ {{ "obsidian", "obsidian", "obsidian", "obsidian", "obsidian", "obsidian" }},
    /* OBSIDIAN_PIPE */ {{ "obsidian_pipe_end", "obsidian_pipe_end", "obsidian_pipe", "obsidian_pipe", "obsidian_pipe", "obsidian_pipe" }},
    /* FIRE        */ {{ "fire", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* LAUNCHER    */ {{ "launcher_face", "launcher_back", "launcher_side", "launcher_side", "launcher_side", "launcher_side" }},
    /* HAY_BALE    */ {{ "hay_bale_top", "hay_bale_top", "hay_bale_side", "hay_bale_side", "hay_bale_side", "hay_bale_side" }},
    /* HEAT_SENSOR */ {{ "heat_sensor", "sensor_bottom", "heat_sensor", "heat_sensor", "heat_sensor", "heat_sensor" }},
    /* SPRINKLER   */ {{ "sprinkler_side", "sprinkler_head", "sprinkler_side", "sprinkler_side", "sprinkler_side", "sprinkler_side" }},
    /* ASPHALT     */ {{ "asphalt", "asphalt", "asphalt", "asphalt", "asphalt", "asphalt" }},
    /* LIMESTONE   */ {{ "limestone", "limestone", "limestone", "limestone", "limestone", "limestone" }},
    /* CONCRETE    */ {{ "concrete", "concrete", "concrete", "concrete", "concrete", "concrete" }},
    /* GRAVEL      */ {{ "gravel", "gravel", "gravel", "gravel", "gravel", "gravel" }},
    /* REINFORCED_CONCRETE */ {{ "reinforced_concrete", "reinforced_concrete", "reinforced_concrete", "reinforced_concrete", "reinforced_concrete", "reinforced_concrete" }},
    /* DEEPSTONE   */ {{ "deepstone", "deepstone", "deepstone", "deepstone", "deepstone", "deepstone" }},
};

// What a block shows where it's been carved into (chiselled faces inside its cell),
// for blocks whose inside differs from their outside: a log's wood under the bark.
const char* CarvedTextureName(BlockType type) {
    return type == BlockType::LOG ? "log_inside" : nullptr;
}

// Resolved atlas indices. Written on the main thread on (re)load, read by the
// mesher on the worker thread, hence atomic.
std::atomic<int> gBlockTextureIndices[kBlockTypeCount][6] = {};
std::atomic<int> gCarvedTextureIndices[kBlockTypeCount] = {}; // -1: none

} // namespace

const char* DefaultBlockTexture(BlockType type, int face) {
    const int t = static_cast<int>(type);
    return t >= 0 && t < kBlockTypeCount && face >= 0 && face < 6 ? kBlockTextureNames[t].faces[face] : nullptr;
}

void ResolveBlockTextures(const std::function<int(const std::string&)>& lookup) {
    for (int type = 0; type < kBlockTypeCount; type++) {
        for (int face = 0; face < 6; face++) {
            const auto set = BlockData::Texture(static_cast<BlockType>(type), face);
            const char* name = set ? set->c_str() : kBlockTextureNames[type].faces[face];
            gBlockTextureIndices[type][face] = name ? lookup(name) : 0;
        }
        const char* carved = CarvedTextureName(static_cast<BlockType>(type));
        gCarvedTextureIndices[type] = carved ? lookup(carved) : -1;
    }
}

namespace {
constexpr const char* kBlockIds[kBlockTypeCount] = {
    "air", "grass", "dirt", "stone", "lamp", "log", "leaves", "tall_grass", "flower_red", "flower_yellow", "sand", "water", "pump", "pipe",
    "stone_brick", "mossy_brick", "spikes", "crumbling_brick", "pressure_plate", "sluice", "snow", "dry_grass", "lush_grass", "snowy_grass",
    "pine_leaves", "cactus", "dead_bush", "ice", "pebbles", "planks", "glass", "chest", "bed", "coal_ore", "iron_ore", "crop", "ripe_wheat", "ladder", "door", "door_open", "campfire", "sign", "tnt", "tnt_lit", "sapling", "pine_sapling",
    "water_wheel", "air_pipe", "drill", "shaft", "compressor", "lever", "lever_on", "logic_gate", "signal_lamp", "signal_lamp_on", "air_tank", "valve", "valve_open",
    "piston", "piston_head", "water_sensor", "pressure_sensor", "daylight_sensor", "player_sensor",
    "item_pipe", "smelter", "smelter_lit", "spring",
    "extractor", "filter_pipe", "crafter", "harvester", "drain", "sinkhole", "pressure_gauge",
    "oil_shale", "tar", "fluid_pipe", "fluid_tank", "oil_derrick", "refinery", "engine", "fluid_outlet", "mining_post",
    "steel_frame", "machine_casing", "fire_brick", "multiblock", "multiblock_part",
    "crusher", "glow_fungus", "magma", "gold_ore", "machine_sensor", "boiler", "turbine", "air_diffuser", "copper_ore", "creative_air_tank", "lava", "fluid_valve", "fluid_valve_open", "obsidian", "obsidian_pipe", "fire", "launcher", "hay_bale", "heat_sensor", "sprinkler", "asphalt", "limestone", "concrete", "gravel", "reinforced_concrete", "deepstone",
};
// Every type needs its id: a missing one would read past the table's end.
static_assert(kBlockIds[kBlockTypeCount - 1] != nullptr, "a block type is missing from kBlockIds");
} // namespace

const char* BlockId(BlockType type) {
    const int t = static_cast<int>(type);
    return t >= 0 && t < kBlockTypeCount ? kBlockIds[t] : "?";
}

BlockType BlockFromId(const std::string& id) {
    for (int t = 1; t < kBlockTypeCount; t++)
        if (id == kBlockIds[t]) return static_cast<BlockType>(t);
    return BlockType::COUNT;
}

int GetCarvedTexture(BlockType type) {
    const int t = static_cast<int>(type);
    return t > 0 && t < kBlockTypeCount ? gCarvedTextureIndices[t].load(std::memory_order_relaxed) : -1;
}

TextureData GetTextureData(BlockType type) {
    int t = static_cast<int>(type);
    if (t < 0 || t >= kBlockTypeCount) return {};
    const auto& i = gBlockTextureIndices[t];
    return { i[0].load(std::memory_order_relaxed), i[1].load(std::memory_order_relaxed),
             i[2].load(std::memory_order_relaxed), i[3].load(std::memory_order_relaxed),
             i[4].load(std::memory_order_relaxed), i[5].load(std::memory_order_relaxed) };
}

namespace {

// A log's face, given the axis it runs along (kFaceAxis terms): its ends (the faces
// along its axis) show the end grain, the rest bark, turned so the grain runs along
// the log. A carved face (`carved`, an atlas index; -1 if not) shows the wood inside
// instead, its grain also along the log.
void OrientGrain(VertexData* v, int n, Face face, int grain, const TextureData& textures, int carved = -1) {
    const int f = static_cast<int>(face);
    const bool end = kFaceAxis[f] == grain;
    if (carved >= 0) {
        for (int i = 0; i < n; i++) v[i].textureIndex = carved;
        if (end) return; // cut across the grain
    }
    else if (end) {
        const bool positive = kFaceOffsets[f][grain] > 0;
        for (int i = 0; i < n; i++) v[i].textureIndex = positive ? textures.top : textures.bottom;
        return;
    }
    else {
        for (int i = 0; i < n; i++) v[i].textureIndex = textures.front;
    }
    // The axis the texture's v (its grain, on bark) runs along on this face.
    int vAxis = -1;
    for (int i = 0; i < n && vAxis < 0; i++)
        for (int j = 0; j < n && vAxis < 0; j++) {
            if (std::abs(v[i].tex.x - v[j].tex.x) > 1e-4f || std::abs(v[i].tex.y - v[j].tex.y) < 1e-4f) continue;
            const glm::vec3 d = glm::abs(v[j].pos - v[i].pos);
            vAxis = d.x >= d.y && d.x >= d.z ? 0 : d.y >= d.z ? 1 : 2;
        }
    if (vAxis >= 0 && vAxis != grain)
        for (int i = 0; i < n; i++) std::swap(v[i].tex.x, v[i].tex.y);
}

} // namespace

void Block::AddSteppedFaces(std::vector<uint32_t>& vertices, Face face, int x, int y, int z, const FaceShading& shading, bool outside) const {
    const TextureData textures = GetTextureData(type);
    const int f = static_cast<int>(face);
    const int texture = face == Face::Top ? textures.top : face == Face::Bottom ? textures.bottom : face == Face::Right ? textures.right
                      : face == Face::Left ? textures.left : face == Face::Back ? textures.back : textures.front;
    const glm::vec3 normal(kFaceOffsets[f][0], kFaceOffsets[f][1], kFaceOffsets[f][2]);
    const glm::vec3 origin(x, y, z);
    // Quarter heights in eighths, [qx][qz]: EdgeData's edge 2 is (0,0), 1 is (1,0), 0 is
    // (0,1) and 3 is (1,1).
    const int h[2][2] = { { edgeData.GetTopY(2), edgeData.GetTopY(0) }, { edgeData.GetTopY(1), edgeData.GetTopY(3) } };
    const auto& tangents = kFaceTangents[kFaceAxis[f]];

    // One rectangle, given its corners as seen from outside: a bottom left, b bottom
    // right, c top left, d top right (in eighths, block-local).
    // Faces inside the cell (a quarter's top below the block's, the step up to a taller
    // quarter) are carved: some blocks show their inside there.
    const int carvedTexture = GetCarvedTexture(type);
    auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, bool carved) {
        const glm::vec3 corners[4] = { a, b, c, d };
        VertexData v[4];
        for (int i = 0; i < 4; i++) {
            const glm::vec3 p = corners[i] / 8.0f;
            // Texture coordinates follow the face, so quarters show their part of it.
            glm::vec2 uv;
            switch (face) {
            case Face::Right: uv = { 1.0f - p.z, 1.0f - p.y }; break;
            case Face::Left: uv = { p.z, 1.0f - p.y }; break;
            case Face::Back: uv = { p.x, 1.0f - p.y }; break;
            case Face::Front: uv = { 1.0f - p.x, 1.0f - p.y }; break;
            default: uv = { p.x, p.z }; break;
            }
            v[i] = VertexData(origin + p, uv, texture, normal);
            // Shade from the face's corner the vertex is nearest (quarters share the
            // whole face's light and ambient occlusion).
            auto high = [&](int axis) { return axis == 1 ? p.y > 0.5f : p[axis] > 0.5f; };
            const int ia = high(tangents[0]), ib = high(tangents[1]);
            v[i].ao = shading.ao[ia][ib];
            v[i].light = shading.light[ia][ib];
            v[i].sky = shading.sky[ia][ib];
        }
        const int inside = carved ? carvedTexture : -1;
        if (((axis & 7) != kUpright && IsOrientable(type)) || inside >= 0) OrientGrain(v, 4, face, GrainAxis(), textures, inside);
        // (The engine's front faces wind clockwise as seen from outside.)
        for (const VertexData& p : { v[0], v[2], v[1], v[1], v[2], v[3] }) {
            vertices.push_back(p.PackPosition());
            vertices.push_back(p.PackShading());
        }
    };

    for (int qx = 0; qx < 2; qx++)
        for (int qz = 0; qz < 2; qz++) {
            const int top = h[qx][qz];
            if (top == 0) continue;
            const float x0 = qx * 4.0f, x1 = x0 + 4.0f, z0 = qz * 4.0f, z1 = z0 + 4.0f, t = static_cast<float>(top);
            switch (face) {
            case Face::Top: quad({ x0, t, z1 }, { x1, t, z1 }, { x0, t, z0 }, { x1, t, z0 }, top < 8); break;
            case Face::Bottom: if (outside) quad({ x0, 0, z0 }, { x1, 0, z0 }, { x0, 0, z1 }, { x1, 0, z1 }, false); break;
            default: {
                // The side of this quarter facing that way: on the block's outside, all
                // of it; inside, only what rises above the quarter next to it.
                const int dx = kFaceOffsets[f][0], dz = kFaceOffsets[f][2];
                const int nx = qx + dx, nz = qz + dz;
                const bool atEdge = nx < 0 || nx > 1 || nz < 0 || nz > 1;
                if (atEdge && !outside) break;
                const float from = atEdge ? 0.0f : static_cast<float>(h[nx][nz]);
                if (from >= t) break;
                switch (face) {
                // Sides on the block's outside are its own (bark); inside, carved.
                case Face::Right: quad({ x1, from, z1 }, { x1, from, z0 }, { x1, t, z1 }, { x1, t, z0 }, !atEdge); break;
                case Face::Left: quad({ x0, from, z0 }, { x0, from, z1 }, { x0, t, z0 }, { x0, t, z1 }, !atEdge); break;
                case Face::Back: quad({ x0, from, z1 }, { x1, from, z1 }, { x0, t, z1 }, { x1, t, z1 }, !atEdge); break;
                default: quad({ x1, from, z0 }, { x0, from, z0 }, { x1, t, z0 }, { x0, t, z0 }, !atEdge); break; // Front
                }
            }
            }
        }
}

void Block::AddBoxVertices(std::vector<uint32_t>& vertices, int x, int y, int z, const glm::ivec3& lo, const glm::ivec3& hi, unsigned skip, uint8_t blockLight,
                           uint8_t skyLight) const {
    const TextureData textures = GetTextureData(type);
    const glm::vec3 origin(x, y, z);
    const float x0 = lo.x, x1 = hi.x, y0 = lo.y, y1 = hi.y, z0 = lo.z, z1 = hi.z;
    for (int f = 0; f < kFaceCount; f++) {
        if (skip & (1u << f)) continue;
        const Face face = static_cast<Face>(f);
        int texture = face == Face::Top ? textures.top : face == Face::Bottom ? textures.bottom : face == Face::Right ? textures.right
                    : face == Face::Left ? textures.left : face == Face::Back ? textures.back : textures.front;
        if (IsFacingType(type)) texture = f == axis % 6 ? textures.top : f == ((axis % 6) ^ 1) ? textures.bottom : textures.front;
        const glm::vec3 normal(kFaceOffsets[f][0], kFaceOffsets[f][1], kFaceOffsets[f][2]);
        // Corners as seen from outside: bottom left, bottom right, top left, top right.
        glm::vec3 c[4];
        switch (face) {
        case Face::Top: c[0] = { x0, y1, z1 }, c[1] = { x1, y1, z1 }, c[2] = { x0, y1, z0 }, c[3] = { x1, y1, z0 }; break;
        case Face::Bottom: c[0] = { x0, y0, z0 }, c[1] = { x1, y0, z0 }, c[2] = { x0, y0, z1 }, c[3] = { x1, y0, z1 }; break;
        case Face::Right: c[0] = { x1, y0, z1 }, c[1] = { x1, y0, z0 }, c[2] = { x1, y1, z1 }, c[3] = { x1, y1, z0 }; break;
        case Face::Left: c[0] = { x0, y0, z0 }, c[1] = { x0, y0, z1 }, c[2] = { x0, y1, z0 }, c[3] = { x0, y1, z1 }; break;
        case Face::Back: c[0] = { x0, y0, z1 }, c[1] = { x1, y0, z1 }, c[2] = { x0, y1, z1 }, c[3] = { x1, y1, z1 }; break;
        default: c[0] = { x1, y0, z0 }, c[1] = { x0, y0, z0 }, c[2] = { x1, y1, z0 }, c[3] = { x0, y1, z0 }; break;
        }
        VertexData v[4];
        for (int i = 0; i < 4; i++) {
            const glm::vec3 p = c[i] / 8.0f;
            glm::vec2 uv;
            switch (face) {
            case Face::Right: uv = { 1.0f - p.z, 1.0f - p.y }; break;
            case Face::Left: uv = { p.z, 1.0f - p.y }; break;
            case Face::Back: uv = { p.x, 1.0f - p.y }; break;
            case Face::Front: uv = { 1.0f - p.x, 1.0f - p.y }; break;
            default: uv = { p.x, p.z }; break;
            }
            v[i] = VertexData(origin + p, uv, texture, normal);
            v[i].ao = 3;
            v[i].light = blockLight;
            v[i].sky = skyLight;
        }
        for (const VertexData& p : { v[0], v[2], v[1], v[1], v[2], v[3] }) {
            vertices.push_back(p.PackPosition());
            vertices.push_back(p.PackShading());
        }
    }
}

void Block::AddFaceVertices(std::vector<uint32_t>& vertices, Face face, int x, int y, int z, const FaceShading& shading) const {
    auto v = GetFaceVertices(face, edgeData, x, y, z, GetTextureData(type));
    if (IsFacingType(type)) {
        // The face it points to, its back, or a side.
        const TextureData t = GetTextureData(type);
        const int f = static_cast<int>(face), towards = axis % 6;
        const int texture = f == towards ? t.top : f == (towards ^ 1) ? t.bottom : t.front;
        for (auto& vertex : v) vertex.textureIndex = texture;
    }
    else if ((axis & 7) != kUpright && IsOrientable(type)) OrientGrain(v.data(), static_cast<int>(v.size()), face, GrainAxis(), GetTextureData(type));

    // Work out which face corner each vertex sits on. Horizontal coordinates are
    // always 0 or 1 within the block; vertical ones may be partial heights, so any
    // vertex above the block's base counts as a top corner.
    const glm::vec3 origin(x, y, z);
    const auto& tangents = kFaceTangents[kFaceAxis[static_cast<int>(face)]];
    auto isHigh = [&](const glm::vec3& pos, int axis) {
        float rel = pos[axis] - origin[axis];
        return axis == 1 ? rel > 0.001f : rel > 0.5f;
    };
    for (auto& vertex : v) {
        const int a = isHigh(vertex.pos, tangents[0]);
        const int b = isHigh(vertex.pos, tangents[1]);
        vertex.ao = shading.ao[a][b];
        vertex.light = shading.light[a][b];
        vertex.sky = shading.sky[a][b];
    }

    // The quad is two triangles (p1, p2, p3) and (p3, p2, p4) sharing the p2-p3
    // diagonal. Interpolating AO across that diagonal gives visible creases when
    // the darker corners lie on it, so for flat (full block) faces split along
    // p1-p4 instead when that pair is brighter.
    const VertexData p1 = v[0], p2 = v[1], p3 = v[2], p4 = v[5];
    if (IsFullBlock() && p1.ao + p4.ao > p2.ao + p3.ao) {
        v = { p1, p2, p4, p4, p3, p1 };
    }

    for (const auto& vertex : v) {
        vertices.push_back(vertex.PackPosition());
        vertices.push_back(vertex.PackShading());
    }
}

namespace {
// A partial block's four floor columns for water (see Block::WaterVolumeAt): heights
// in units and weights (out of `total`).
struct WaterColumns {
    int height[4];
    int weight[4];
    int total;
};
WaterColumns ColumnsOf(const Block& b) {
    constexpr int unitsPerEighth = Block::kMaxWaterVolume / 8;
    WaterColumns c;
    const EdgeData edges = b.WaterEdges();
    if (b.WaterStepped()) {
        for (int e = 0; e < 4; e++) c.height[e] = edges.GetTopY(e) * unitsPerEighth, c.weight[e] = 1;
        c.total = 4;
        return c;
    }
    // Corners (edge 2 is (0,0), 1 is (1,0), 0 is (0,1), 3 is (1,1)); the surface is
    // split along the (0,1)-(1,0) diagonal, so those two corners count twice.
    const int weights[4] = { 2, 2, 1, 1 }; // edges 0, 1, 2, 3
    for (int e = 0; e < 4; e++) c.height[e] = edges.GetTopY(e) * unitsPerEighth, c.weight[e] = weights[e];
    c.total = 6;
    return c;
}
} // namespace

int Block::WaterVolumeAt(int level) const {
    const int capacity = WaterCapacity();
    if (!IsPartial()) return std::clamp(level, 0, capacity);
    if (level >= kMaxWaterVolume) return capacity;
    const WaterColumns c = ColumnsOf(*this);
    int sum = 0;
    for (int e = 0; e < 4; e++) sum += c.weight[e] * std::clamp(level - c.height[e], 0, kMaxWaterVolume - c.height[e]);
    return std::min(capacity, sum / c.total);
}

int Block::WaterLevelFor(int volume) const {
    if (!IsPartial()) return volume;
    const WaterColumns c = ColumnsOf(*this);
    int lo = kMaxWaterVolume;
    for (int e = 0; e < 4; e++) lo = std::min(lo, c.height[e]);
    if (volume <= 0) return lo; // dry: its lowest point, where water would go first
    if (volume >= WaterCapacity()) return kMaxWaterVolume;
    // The highest level this much water still reaches: in a narrow notch one unit of
    // water spans several levels, and it stands at the top of them.
    int hi = kMaxWaterVolume;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (WaterVolumeAt(mid) <= volume) lo = mid; else hi = mid - 1;
    }
    return lo;
}

uint8_t Block::WaterFloor() const {
    if (!IsPartial()) return 0;
    constexpr int unitsPerEighth = kMaxWaterVolume / 8;
    if (WaterStepped()) {
        // Four quarters: their mean height.
        const EdgeData edges = WaterEdges();
        int sum = 0;
        for (int e = 0; e < 4; e++) sum += edges.GetTopY(e);
        return static_cast<uint8_t>((sum * unitsPerEighth + 2) / 4);
    }
    // Mean height of the two top triangles (see SurfaceHeight), in units.
    const int c00 = edgeData.GetTopY(2), c10 = edgeData.GetTopY(1), c01 = edgeData.GetTopY(0), c11 = edgeData.GetTopY(3);
    const int sixths = c00 + c11 + 2 * (c10 + c01);           // 6 * mean, in eighths
    return static_cast<uint8_t>((sixths * unitsPerEighth + 3) / 6);
}

float Block::SurfaceHeight(float fx, float fz) const {
    if (IsPassable()) return 0.0f;
    if (IsFullBlock()) return 1.0f;

    // Corner heights. See GetFaceVertices(Face::Top): edge 2 is (0,0), 1 is (1,0),
    // 0 is (0,1) and 3 is (1,1), and the quad is split along the (0,1)-(1,0) diagonal.
    const float c00 = edgeData.GetTopY(2) / 8.0f;
    const float c10 = edgeData.GetTopY(1) / 8.0f;
    const float c01 = edgeData.GetTopY(0) / 8.0f;
    const float c11 = edgeData.GetTopY(3) / 8.0f;
    if (IsStepped()) return fx < 0.5f ? (fz < 0.5f ? c00 : c01) : (fz < 0.5f ? c10 : c11); // the quarter's height

    if (fx + fz <= 1.0f) return c00 + (c10 - c00) * fx + (c01 - c00) * fz;
    return c11 + (c01 - c11) * (1.0f - fx) + (c10 - c11) * (1.0f - fz);
}

int Block::PebbleStones(int x, int y, int z, glm::vec3 lo[5], glm::vec3 hi[5]) const {
    // Four or five little stones, an eighth high, one or two eighths across.
    const float bottom = -PlantDrop();
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(z) * 19349663u ^ static_cast<uint32_t>(y) * 83492791u;
    auto next = [&](uint32_t range) {
        h ^= h << 13;
        h ^= h >> 17;
        h ^= h << 5;
        return h % range;
    };
    const int count = 4 + static_cast<int>(next(2));
    for (int i = 0; i < count; i++) {
        const int w = 1 + static_cast<int>(next(2)), d = 1 + static_cast<int>(next(2)); // eighths
        const int px = 1 + static_cast<int>(next(static_cast<uint32_t>(7 - w))), pz = 1 + static_cast<int>(next(static_cast<uint32_t>(7 - d)));
        lo[i] = glm::vec3(px / 8.0f, bottom, pz / 8.0f);
        hi[i] = glm::vec3(lo[i].x + w / 8.0f, bottom + 1.0f / 8.0f, lo[i].z + d / 8.0f);
    }
    return count;
}

namespace {
constexpr float kShapeEpsilon = 1e-4f;

// Whether (lo, hi) overlaps the block's shape as if it were upright (lo, hi in that
// frame), and the highest point of that upright shape over a footprint.
bool UprightOverlaps(const Block& b, glm::vec3 lo, glm::vec3 hi) {
    lo = glm::max(lo, glm::vec3(0.0f));
    hi = glm::min(hi, glm::vec3(1.0f));
    if (lo.x >= hi.x - kShapeEpsilon || lo.z >= hi.z - kShapeEpsilon || lo.y >= hi.y - kShapeEpsilon) return false;
    if (b.IsStepped()) {
        // Quarter [qx][qz] is EdgeData edge kEdgeAt[qx][qz].
        constexpr int kEdgeAt[2][2] = { { 2, 0 }, { 1, 3 } };
        for (int qx = 0; qx < 2; qx++)
            for (int qz = 0; qz < 2; qz++) {
                const int e = kEdgeAt[qx][qz];
                const float top = b.edgeData.GetTopY(e) / 8.0f, bottom = b.edgeData.GetBottomY(e) / 8.0f;
                if (top <= bottom) continue;
                if (std::max(lo.x, qx * 0.5f) < std::min(hi.x, qx * 0.5f + 0.5f) - kShapeEpsilon &&
                    std::max(lo.z, qz * 0.5f) < std::min(hi.z, qz * 0.5f + 0.5f) - kShapeEpsilon && lo.y < top - kShapeEpsilon &&
                    hi.y > bottom + kShapeEpsilon)
                    return true;
            }
        return false;
    }
    return lo.y < b.SurfaceMax(lo.x, hi.x, lo.z, hi.z) - kShapeEpsilon;
}

float UprightTop(const Block& b, float x0, float x1, float z0, float z1) {
    x0 = std::max(x0, 0.0f), z0 = std::max(z0, 0.0f), x1 = std::min(x1, 1.0f), z1 = std::min(z1, 1.0f);
    if (x0 >= x1 - kShapeEpsilon || z0 >= z1 - kShapeEpsilon) return -1.0f;
    if (b.IsStepped()) {
        constexpr int kEdgeAt[2][2] = { { 2, 0 }, { 1, 3 } };
        float top = -1.0f;
        for (int qx = 0; qx < 2; qx++)
            for (int qz = 0; qz < 2; qz++) {
                const int e = kEdgeAt[qx][qz];
                if (b.edgeData.GetTopY(e) <= b.edgeData.GetBottomY(e)) continue;
                if (std::max(x0, qx * 0.5f) < std::min(x1, qx * 0.5f + 0.5f) - kShapeEpsilon &&
                    std::max(z0, qz * 0.5f) < std::min(z1, qz * 0.5f + 0.5f) - kShapeEpsilon)
                    top = std::max(top, b.edgeData.GetTopY(e) / 8.0f);
            }
        return top;
    }
    return b.SurfaceMax(x0, x1, z0, z1);
}
} // namespace

float Block::SurfaceMax(float x0, float x1, float z0, float z1) const {
    x0 = std::max(x0, 0.0f), z0 = std::max(z0, 0.0f), x1 = std::min(x1, 1.0f), z1 = std::min(z1, 1.0f);
    if (x0 > x1 || z0 > z1) return -1.0f;
    // The surface is flat on each side of the x + z = 1 diagonal, so its highest point over
    // a rectangle is at a corner of the rectangle or where the diagonal crosses its edges.
    float top = std::max(std::max(SurfaceHeight(x0, z0), SurfaceHeight(x1, z0)), std::max(SurfaceHeight(x0, z1), SurfaceHeight(x1, z1)));
    for (const float x : { x0, x1 })
        if (const float z = 1.0f - x; z >= z0 && z <= z1) top = std::max(top, SurfaceHeight(x, z));
    for (const float z : { z0, z1 })
        if (const float x = 1.0f - z; x >= x0 && x <= x1) top = std::max(top, SurfaceHeight(x, z));
    return top;
}

bool Block::ShapeOverlaps(const glm::vec3& lo, const glm::vec3& hi) const {
    if (IsFullBlock()) return glm::all(glm::lessThan(lo, glm::vec3(1.0f - kShapeEpsilon))) && glm::all(glm::greaterThan(hi, glm::vec3(kShapeEpsilon)));
    if (!IsPartial()) return false;
    const int dir = ShapeDir();
    if (dir == 0) return UprightOverlaps(*this, lo, hi);
    // Turned a quarter or a half turn at a time, a box is still a box in the shape's frame.
    const glm::vec3 a = CellToShape(dir, lo), b = CellToShape(dir, hi);
    return UprightOverlaps(*this, glm::min(a, b), glm::max(a, b));
}

float Block::ShapeTop(float x0, float x1, float z0, float z1) const {
    if (IsFullBlock()) return (x0 < 1.0f && x1 > 0.0f && z0 < 1.0f && z1 > 0.0f) ? 1.0f : -1.0f;
    if (!IsPartial()) return -1.0f;
    const int dir = ShapeDir();
    if (dir == 0) return UprightTop(*this, x0, x1, z0, z1);
    // Upside down: it hangs from the top of the cell, wherever it's over the footprint.
    if (dir == 3) return ShapeOverlaps({ x0, 0.0f, z0 }, { x1, 1.0f, z1 }) ? 1.0f : -1.0f;
    // On its side: find the highest slice with some of it in, then the top within that
    // slice (below the top it's there, above it it isn't).
    constexpr int kSlices = 64;
    auto within = [&](float y0, float y1) { return ShapeOverlaps({ x0, y0, z0 }, { x1, y1, z1 }); };
    for (int k = kSlices; k > 0; k--) {
        const float sliceTop = k / static_cast<float>(kSlices), sliceBottom = (k - 1) / static_cast<float>(kSlices);
        if (!within(sliceBottom, sliceTop)) continue;
        float lo = sliceBottom, hi = sliceTop;
        for (int i = 0; i < 10; i++) {
            const float mid = (lo + hi) * 0.5f;
            if (within(mid, sliceTop)) lo = mid; else hi = mid;
        }
        return lo;
    }
    return -1.0f;
}

EdgeData Block::WaterEdges() const {
    if (ShapeDir() == 0) return edgeData;
    // Each quarter as high as the most of it the turned shape fills, rounded up to eighths.
    constexpr int kEdgeAt[2][2] = { { 2, 0 }, { 1, 3 } };
    EdgeData edges;
    for (int qx = 0; qx < 2; qx++)
        for (int qz = 0; qz < 2; qz++) {
            const float top = ShapeTop(qx * 0.5f, qx * 0.5f + 0.5f, qz * 0.5f, qz * 0.5f + 0.5f);
            const int e = kEdgeAt[qx][qz];
            edges.SetBottomY(e, 0);
            edges.SetTopY(e, static_cast<uint8_t>(top <= 0.0f ? 0 : std::clamp(static_cast<int>(std::ceil(top * 8.0f - 0.01f)), 0, 8)));
        }
    return edges;
}

Block Block::Upright() const {
    const int dir = ShapeDir();
    Block b = *this;
    if (dir == 0) return b;
    if (!KeepsGrain(type)) {
        b.axis = 0;
        return b;
    }
    // The grain, as it runs in the shape's own frame.
    glm::vec3 grain(0.0f);
    grain[GrainAxis()] = 1.0f;
    const glm::vec3 g = glm::abs(CellVectorToShape(dir, grain));
    b.axis = g.x > 0.5f ? kAlongX : g.z > 0.5f ? kAlongZ : kUpright;
    return b;
}

void Block::AddPlantVertices(std::vector<uint32_t>& vertices, int x, int y, int z, uint8_t blockLight, uint8_t skyLight) const {
    if (type == BlockType::PEBBLES) return AddPebbleVertices(vertices, x, y, z, blockLight, skyLight);
    const float bottom = y - PlantDrop();
    const float top = bottom + 1.0f;
    const int texture = GetTextureData(type).top;
    const glm::vec3 up(0.0f, 1.0f, 0.0f); // lit like an upward face: plants catch the sun

    auto vertex = [&](float vx, float vy, float vz, float u, float v) {
        VertexData d(glm::vec3(vx, vy, vz), glm::vec2(u, v), texture, up);
        d.light = blockLight;
        d.sky = skyLight;
        return d;
    };

    // Two quads along the cell's diagonals. v = 0 is the top of the texture.
    const float diagonals[2][4] = { { 0, 0, 1, 1 }, { 1, 0, 0, 1 } }; // x0, z0, x1, z1
    for (const auto& d : diagonals) {
        VertexData bl = vertex(x + d[0], bottom, z + d[1], 0, 1);
        VertexData br = vertex(x + d[2], bottom, z + d[3], 1, 1);
        VertexData tl = vertex(x + d[0], top, z + d[1], 0, 0);
        VertexData tr = vertex(x + d[2], top, z + d[3], 1, 0);
        // Both windings so the sprite shows from either side with back-face culling on.
        const VertexData quad[12] = { bl, tl, br, br, tl, tr,   bl, br, tl, br, tr, tl };
        for (const auto& q : quad) {
            vertices.push_back(q.PackPosition());
            vertices.push_back(q.PackShading());
        }
    }
}

void Block::AddPebbleVertices(std::vector<uint32_t>& vertices, int x, int y, int z, uint8_t blockLight, uint8_t skyLight) const {
    // Four or five little stones, an eighth high, one or two eighths across, placed
    // by the cell's position (so neighbouring cells differ), on the ground the plant
    // drop says.
    const int texture = GetTextureData(type).bottom; // stone
    auto vertex = [&](const glm::vec3& p, float u, float v, const glm::vec3& normal) {
        VertexData d(p, glm::vec2(u, v), texture, normal);
        d.light = blockLight;
        d.sky = skyLight;
        return d;
    };
    glm::vec3 stoneLo[5], stoneHi[5];
    const int count = PebbleStones(x, y, z, stoneLo, stoneHi);
    for (int i = 0; i < count; i++) {
        const glm::vec3 lo = glm::vec3(x, y, z) + stoneLo[i], hi = glm::vec3(x, y, z) + stoneHi[i];
        const float u0 = stoneLo[i].x, v0 = stoneLo[i].z, u1 = stoneHi[i].x, v1 = stoneHi[i].z; // their bit of the texture
        auto quad = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& e, const glm::vec3& n) {
            const VertexData q[4] = { vertex(a, u0, v1, n), vertex(b, u1, v1, n), vertex(c, u0, v0, n), vertex(e, u1, v0, n) };
            // Both windings (like plants): small enough not to matter.
            for (const VertexData& p : { q[0], q[2], q[1], q[1], q[2], q[3], q[0], q[1], q[2], q[1], q[3], q[2] }) {
                vertices.push_back(p.PackPosition());
                vertices.push_back(p.PackShading());
            }
        };
        quad({ lo.x, hi.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, hi.z }, { hi.x, hi.y, hi.z }, { 0, 1, 0 }); // top
        quad({ lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { lo.x, hi.y, lo.z }, { hi.x, hi.y, lo.z }, { 0, 0, -1 });
        quad({ lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { lo.x, hi.y, hi.z }, { hi.x, hi.y, hi.z }, { 0, 0, 1 });
        quad({ lo.x, lo.y, lo.z }, { lo.x, lo.y, hi.z }, { lo.x, hi.y, lo.z }, { lo.x, hi.y, hi.z }, { -1, 0, 0 });
        quad({ hi.x, lo.y, lo.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, lo.z }, { hi.x, hi.y, hi.z }, { 1, 0, 0 });
    }
}

namespace {

// A point in a shape drawn pointing +z, in the cell of a block pointing `d`.
glm::vec3 TurnTo(const glm::ivec3& d, const glm::vec3& p) {
    if (d.x > 0) return { p.z, p.y, 1.0f - p.x };
    if (d.x < 0) return { 1.0f - p.z, p.y, p.x };
    if (d.y > 0) return { p.x, p.z, 1.0f - p.y };
    if (d.y < 0) return { p.x, 1.0f - p.z, p.y };
    if (d.z < 0) return { 1.0f - p.x, p.y, 1.0f - p.z };
    return p;
}

} // namespace

int Block::ModelBoxes(unsigned joined, glm::vec3 lo[kMaxModelBoxes], glm::vec3 hi[kMaxModelBoxes]) const {
    const BlockData::Shape& shape = BlockData::ShapeOf(type);
    if (!shape.hasBoxes) return -1;
    const glm::ivec3 d = shape.turns ? Pointing() : glm::ivec3(0, 0, 1);
    const bool mirror = shape.mirrors && DoorMirrored();
    int n = 0;
    bool runKept = false; // (a box, or one of the "otherwise" ones after it, counted)
    for (const BlockData::ShapeBox& b : shape.boxes) {
        bool keep = (joined & b.joinedAll) == b.joinedAll && (!b.joinedAny || (joined & b.joinedAny)) && !(joined & b.joinedNone);
        if (b.otherwise) {
            keep = keep && !runKept;
            runKept = runKept || keep;
        } else runKept = keep;
        if (!keep || n == kMaxModelBoxes) continue;
        glm::vec3 a = b.lo, c = b.hi;
        if (mirror) a.x = 1.0f - b.hi.x, c.x = 1.0f - b.lo.x;
        a = TurnTo(d, a), c = TurnTo(d, c);
        lo[n] = glm::min(a, c), hi[n] = glm::max(a, c);
        n++;
    }
    return n;
}

bool Block::Collides() const {
    if (IsAir() || IsLiquid() || IsLava()) return false;
    return BlockData::ShapeOf(type).solid.value_or(!IsPlant());
}

bool Block::StoodOn() const { return Collides() && BlockData::ShapeOf(type).floor.value_or(true); }

std::vector<glm::vec3> Block::OutlineSegments(const glm::ivec3& cell, unsigned joined) const {
    if (const int dir = ShapeDir(); dir != 0 && IsPartial()) {
        // Turned: the upright outline, turned into place.
        Block upright = *this;
        upright.axis = 0;
        std::vector<glm::vec3> turned = upright.OutlineSegments();
        for (auto& p : turned) p = ShapeToCell(dir, p);
        return turned;
    }
    std::vector<glm::vec3> lines;
    auto edge = [&](glm::vec3 a, glm::vec3 b) {
        lines.push_back(a);
        lines.push_back(b);
    };
    auto box = [&](glm::vec3 lo, glm::vec3 hi) {
        const glm::vec3 c[8] = { { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, lo.y, hi.z }, { lo.x, lo.y, hi.z },
                                 { lo.x, hi.y, lo.z }, { hi.x, hi.y, lo.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } };
        for (int i = 0; i < 4; i++) {
            edge(c[i], c[(i + 1) % 4]);         // bottom
            edge(c[4 + i], c[4 + (i + 1) % 4]); // top
            edge(c[i], c[4 + i]);               // sides
        }
    };
    if (type == BlockType::PEBBLES) {
        // Each stone (where it's drawn: by the cell's place in its chunk).
        const glm::ivec3 local = Coords::LocalOf(cell);
        glm::vec3 lo[5], hi[5];
        const int count = PebbleStones(local.x, local.y, local.z, lo, hi);
        for (int i = 0; i < count; i++) box(lo[i], hi[i]);
        return lines;
    }
    if (IsPlant() && !IsLeverType(type) && type != BlockType::DOOR_OPEN) {
        // Round its picture (its two crossed sprites, as far as they have anything solid).
        const float drop = PlantDrop();
        float u0 = 0.15f, u1 = 0.85f, height = 0.9f;
        if (SpriteMasks::Bounds(GetTextureData(type).top, u0, u1, height)) {
            const float a = std::min(u0, 1.0f - u1), b = std::max(u1, 1.0f - u0);
            u0 = a, u1 = b;
        }
        box({ u0, -drop, u0 }, { u1, height - drop, u1 });
        return lines;
    }
    {
        // Drawn smaller than its cell: round each of its boxes.
        glm::vec3 lo[kMaxModelBoxes], hi[kMaxModelBoxes];
        const int count = ModelBoxes(joined, lo, hi);
        for (int i = 0; i < count; i++) box(lo[i], hi[i]);
        if (count >= 0) return lines;
    }
    if (IsFullBlock() || IsLiquid()) {
        box(glm::vec3(0.0f), glm::vec3(1.0f));
        return lines;
    }
    // Corner heights at (0,0), (1,0), (1,1), (0,1) (EdgeData edges 2, 1, 3, 0).
    const float h[4] = { edgeData.GetTopY(2) / 8.0f, edgeData.GetTopY(1) / 8.0f, edgeData.GetTopY(3) / 8.0f, edgeData.GetTopY(0) / 8.0f };
    if (IsStepped()) {
        // One box when all four quarters match (a slab, a plate), else each quarter.
        if (h[0] == h[1] && h[1] == h[2] && h[2] == h[3]) {
            box(glm::vec3(0.0f), glm::vec3(1.0f, h[0], 1.0f));
            return lines;
        }
        const glm::vec2 q[4] = { { 0, 0 }, { 0.5f, 0 }, { 0.5f, 0.5f }, { 0, 0.5f } };
        for (int i = 0; i < 4; i++)
            if (h[i] > 0.0f) box({ q[i].x, 0.0f, q[i].y }, { q[i].x + 0.5f, h[i], q[i].y + 0.5f });
        return lines;
    }
    // A slope: its base, the corners up to their heights, the sloped top's edges, and
    // the fold between its two triangles (along (0,1)-(1,0); see GetFaceVertices).
    const glm::vec2 c[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (int i = 0; i < 4; i++) {
        const int j = (i + 1) % 4;
        edge({ c[i].x, 0.0f, c[i].y }, { c[j].x, 0.0f, c[j].y });
        edge({ c[i].x, h[i], c[i].y }, { c[j].x, h[j], c[j].y });
        if (h[i] > 0.0f) edge({ c[i].x, 0.0f, c[i].y }, { c[i].x, h[i], c[i].y });
    }
    if (std::abs((h[0] + h[2]) - (h[1] + h[3])) > 0.001f) edge({ 1.0f, h[1], 0.0f }, { 0.0f, h[3], 1.0f });
    return lines;
}

void Block::Shrink() {
    edgeData.Shrink();
    if (!edgeData.IsValid()) {
        type = BlockType::AIR;
        edgeData.MakeFull();
    }
}

int Block::ResourceUnits() const {
    if (IsAir() || IsLiquid() || IsLava() || IsPartOnlyType(type)) return 0;
    if (type == BlockType::PEBBLES) return kPebbleUnits;
    if (IsFullBlock() || !CanChisel()) return kUnitsPerBlock;
    if (IsStepped()) {
        // Four quarters: each eighth of a quarter's height is 1/32 of the block.
        int eighths = 0;
        for (int e = 0; e < 4; e++) eighths += edgeData.GetTopY(e);
        return eighths * kUnitsPerBlock / 32;
    }
    // A slope's volume is its mean height (the two triangles it's drawn as).
    const int c00 = edgeData.GetTopY(2), c10 = edgeData.GetTopY(1), c01 = edgeData.GetTopY(0), c11 = edgeData.GetTopY(3);
    const int sixths = c00 + c11 + 2 * (c10 + c01); // 6 * mean height, in eighths
    return (sixths * kUnitsPerBlock + 24) / 48;
}

bool Block::CanChisel() const {
    if (IsAir() || IsLiquid() || IsLava() || IsPlant()) return false;
    switch (type) {
    case BlockType::PUMP:
    case BlockType::PIPE:
    case BlockType::WATER_WHEEL:
    case BlockType::AIR_PIPE:
    case BlockType::DRILL:
    case BlockType::SHAFT:
    case BlockType::COMPRESSOR:
    case BlockType::LOGIC_GATE:
    case BlockType::SIGNAL_LAMP:
    case BlockType::SIGNAL_LAMP_ON:
    case BlockType::AIR_TANK:
    case BlockType::CREATIVE_AIR_TANK:
    case BlockType::VALVE:
    case BlockType::VALVE_OPEN:
    case BlockType::FLUID_VALVE:
    case BlockType::FLUID_VALVE_OPEN:
    case BlockType::OBSIDIAN_PIPE:
    case BlockType::PISTON:
    case BlockType::LAUNCHER:
    case BlockType::PISTON_HEAD:
    case BlockType::WATER_SENSOR:
    case BlockType::MACHINE_SENSOR:
    case BlockType::HEAT_SENSOR:
    case BlockType::PRESSURE_SENSOR:
    case BlockType::DAYLIGHT_SENSOR:
    case BlockType::PLAYER_SENSOR:
    case BlockType::ITEM_PIPE:
    case BlockType::SMELTER:
    case BlockType::SMELTER_LIT:
    case BlockType::EXTRACTOR:
    case BlockType::FILTER_PIPE:
    case BlockType::CRAFTER:
    case BlockType::HARVESTER:
    case BlockType::PRESSURE_GAUGE:
    case BlockType::FLUID_PIPE:
    case BlockType::FLUID_TANK:
    case BlockType::OIL_DERRICK:
    case BlockType::REFINERY:
    case BlockType::ENGINE:
    case BlockType::BOILER:
    case BlockType::TURBINE:
    case BlockType::AIR_DIFFUSER:
    case BlockType::FLUID_OUTLET:
    case BlockType::SPRINKLER:
    case BlockType::MULTIBLOCK:
    case BlockType::MULTIBLOCK_PART:
    case BlockType::CRUSHER:
    case BlockType::SLUICE:
    case BlockType::PRESSURE_PLATE:
    case BlockType::GLASS:
    case BlockType::CHEST:
    case BlockType::DOOR:
    case BlockType::TNT:
    case BlockType::TNT_LIT:
    case BlockType::OIL_SHALE:   // (its axis counts what a derrick has drawn, not a shape's turn)
    case BlockType::MINING_POST: // (it holds things, like a chest)
    case BlockType::BED: return false;
    default: return true;
    }
}

Block Block::Chiselled(int edge, int deltaEighths) const {
    Block b = *this;
    const int height = std::clamp(edgeData.GetTopY(edge) + deltaEighths, 0, 8);
    b.edgeData.SetTopY(edge, static_cast<uint8_t>(height));
    if (b.edgeData.IsFull()) {
        b.water = b.shown = 0; // a full block holds no water
        b.SetShapeDir(0);      // (and, whole again, isn't turned any way)
        return b;
    }
    int remaining = 0;
    for (int e = 0; e < 4; e++) remaining += b.edgeData.GetTopY(e);
    if (remaining == 0) return HasWater() ? Water(water) : Block(); // nothing left: its water stays
    b.SetEdgeData(b.edgeData);
    if (b.IsAir()) return HasWater() ? Water(water) : Block(); // (too little left to stand: as above)
    return b;
}

void Block::SetEdgeData(EdgeData edges) {
    // A stepped block stands as long as any quarter does; a slope needs at least two
    // corners above its base.
    bool valid = edges.IsValid();
    if (IsStepped())
        for (int e = 0; e < 4; e++) valid |= edges.GetTopY(e) > edges.GetBottomY(e);
    if (valid) {
        edgeData = edges;
    }
    else {
        type = BlockType::AIR;
        edgeData.MakeFull();
    }
}
