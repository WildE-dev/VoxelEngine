#include "world/Multiblocks.h"

#include <algorithm>

namespace Multiblocks {
namespace {

// A pattern from layers of rows (y up; each layer's rows are z 0..2, each row x 0..2),
// with a legend for the characters ('.' is anything: not part of it).
struct Legend {
    char c;
    BlockType type;
    Role role;
};

Pattern Build(Kind kind, const char* id, const char* name, const std::vector<std::array<const char*, 3>>& layers, const std::vector<Legend>& legend) {
    Pattern p{ kind, id, name, {} };
    for (int y = 0; y < static_cast<int>(layers.size()); y++)
        for (int z = 0; z < 3; z++)
            for (int x = 0; x < 3; x++) {
                const char c = layers[y][z][x];
                if (c == '.') continue;
                for (const auto& l : legend)
                    if (l.c == c) p.cells.push_back({ glm::ivec3(x, y, z), l.type, l.role });
            }
    return p;
}

const std::vector<Pattern>& All() {
    static const std::vector<Pattern> patterns = {
        // F steel frame, D the derrick, A its air pipe, L its crude out (a fluid pipe).
        Build(Kind::OilDerrick, "oil_derrick", "Oil derrick",
              { { "FFF", "ADL", "FFF" }, { "F.F", "...", "F.F" }, { "F.F", "...", "F.F" }, { ".F.", "FFF", ".F." }, { "...", ".F.", "..." } },
              { { 'F', BlockType::STEEL_FRAME, Role::Part }, { 'D', BlockType::OIL_DERRICK, Role::Core },
                { 'A', BlockType::AIR_PIPE, Role::AirPort }, { 'L', BlockType::FLUID_PIPE, Role::FluidOut } }),
        // C machine casing, T fluid tank, R the refinery, A air, I crude in, O fuel out.
        Build(Kind::Refinery, "refinery", "Refinery",
              { { "CAC", "IRO", "CCC" }, { "TCT", "CCC", "TCT" }, { ".C.", "CCC", ".C." } },
              { { 'C', BlockType::MACHINE_CASING, Role::Part }, { 'T', BlockType::FLUID_TANK, Role::Part }, { 'R', BlockType::REFINERY, Role::Core },
                { 'A', BlockType::AIR_PIPE, Role::AirPort }, { 'I', BlockType::FLUID_PIPE, Role::FluidIn }, { 'O', BlockType::FLUID_PIPE, Role::FluidOut } }),
        // B fire bricks, S the smelter (inside), A air, H the item pipe to its chests.
        Build(Kind::BlastFurnace, "blast_furnace", "Blast furnace",
              { { "BAB", "BBB", "BBB" }, { "BHB", "BSB", "BBB" }, { "BBB", "B.B", "BBB" } },
              { { 'B', BlockType::FIRE_BRICK, Role::Part }, { 'S', BlockType::SMELTER, Role::Core },
                { 'A', BlockType::AIR_PIPE, Role::AirPort }, { 'H', BlockType::ITEM_PIPE, Role::ItemPort } }),
        // M machine casing, T fluid tank, E the engine, A air out, F fuel in.
        Build(Kind::DieselCompressor, "diesel_compressor", "Diesel compressor",
              { { "MAM", "FEM", "MMM" }, { "TMT", "MMM", "TMT" } },
              { { 'M', BlockType::MACHINE_CASING, Role::Part }, { 'T', BlockType::FLUID_TANK, Role::Part }, { 'E', BlockType::ENGINE, Role::Core },
                { 'A', BlockType::AIR_PIPE, Role::AirPort }, { 'F', BlockType::FLUID_PIPE, Role::FluidIn } }),
        // S stone bricks, X the smelter (inside), A air, H the item pipe to its chests.
        Build(Kind::CokeOven, "coke_oven", "Coke oven",
              { { "SSS", "HXA", "SSS" }, { "SSS", "SSS", "SSS" }, { "...", ".S.", "..." } },
              { { 'S', BlockType::STONE_BRICK, Role::Part }, { 'X', BlockType::SMELTER, Role::Core },
                { 'A', BlockType::AIR_PIPE, Role::AirPort }, { 'H', BlockType::ITEM_PIPE, Role::ItemPort } }),
    };
    return patterns;
}

} // namespace

const Pattern& PatternOf(Kind k) { return All()[std::min<size_t>(static_cast<size_t>(k), All().size() - 1)]; }

glm::ivec3 OriginOf(const glm::ivec3& corePos, const Block& core) {
    const Pattern& p = PatternOf(KindOf(core));
    for (const auto& c : p.cells)
        if (c.role == Role::Core) return corePos - (Place(glm::ivec3(0), TurnOf(core), c.at));
    return corePos;
}

bool PortOf(const glm::ivec3& corePos, const Block& core, Role role, glm::ivec3& out) {
    const Pattern& p = PatternOf(KindOf(core));
    const glm::ivec3 origin = OriginOf(corePos, core);
    for (const auto& c : p.cells)
        if (c.role == role) {
            out = Place(origin, TurnOf(core), c.at);
            return true;
        }
    return false;
}

} // namespace Multiblocks
