// Reading and writing the recipe files (world/Crafting.h, world/Smelting.h).
//
// recipes.json: a list, in order, of
//   { "id": "planks", "name": "4 planks", "takes": { "log": 1 }, "makes": { "planks": 4 } }
// with amounts in blocks (or items): 0.25 is a quarter. Instead of "makes" a recipe may
// make a tool ("tool": "pickaxe"), a piece of armour ("armour": "cap"), or an upgrade
// ("upgrade": "iron" or "steel": the tool in your hand becomes iron, or steel).
// smelting.json: a list, in order, of
//   { "takes": { "iron_ingot": 1, "coal": 0.5 }, "makes": { "steel_ingot": 1 } }
// (one thing taken, or two).
#include <cmath>
#include <cstdio>
#include <fstream>

#include "world/Crafting.h"
#include "world/Json.h"
#include "world/Smelting.h"

namespace {

uint32_t Units(double blocks) { return static_cast<uint32_t>(std::lround(blocks * Block::kUnitsPerBlock)); }

bool TypeFrom(const std::string& id, BlockType& out, const std::string& where, std::string& error) {
    out = BlockFromId(id);
    if (out == BlockType::COUNT || out == BlockType::AIR) {
        error = where + ": no block or item called '" + id + "'";
        return false;
    }
    return true;
}

// "takes" or "makes": block id -> amount (in blocks).
bool Amounts(const Json::Value* v, std::vector<Ingredient>& out, const std::string& where, std::string& error) {
    if (!v || !v->IsObject()) return true;
    for (const auto& [id, amount] : v->object) {
        BlockType t;
        if (!TypeFrom(id, t, where, error)) return false;
        if (!amount.IsNumber() || Units(amount.number) == 0) {
            error = where + ": '" + id + "' needs an amount (in blocks, more than none)";
            return false;
        }
        out.push_back({ t, Units(amount.number) });
    }
    return true;
}

Json::Value AmountsValue(const std::vector<Ingredient>& in) {
    Json::Value o = Json::Object();
    for (const auto& i : in) Json::Member(o, BlockId(i.type)) = Json::Number(static_cast<double>(i.units) / Block::kUnitsPerBlock);
    return o;
}

bool WriteFile(const std::string& path, const std::string& header, const Json::Value& v) {
    std::ofstream out(path, std::ios::binary);
    out << header << Json::Write(v, 140);
    return static_cast<bool>(out);
}

} // namespace

namespace Crafting {

bool Load(const std::string& path, std::string& error) {
    Json::Value root;
    if (!Json::ParseFile(path, root, error)) return false;
    if (!root.IsArray()) {
        error = path + ": should be a [ ... ] list of recipes";
        return false;
    }
    std::vector<Recipe> table;
    for (size_t i = 0; i < root.array.size(); i++) {
        const Json::Value& v = root.array[i];
        Recipe r;
        r.id = v.StringOr("id", "");
        const std::string where = path + ": recipe " + std::to_string(i + 1) + (r.id.empty() ? "" : " (" + r.id + ")");
        if (r.id.empty()) {
            error = where + ": it needs an id";
            return false;
        }
        r.name = v.StringOr("name", r.id);
        if (!Amounts(v.Find("takes"), r.inputs, where, error)) return false;
        std::vector<Ingredient> makes;
        if (!Amounts(v.Find("makes"), makes, where, error)) return false;
        if (makes.size() > 1) {
            error = where + ": it makes one thing";
            return false;
        }
        if (!makes.empty()) r.output = makes[0].type, r.outputUnits = makes[0].units;
        if (const std::string tool = v.StringOr("tool", ""); !tool.empty()) {
            r.tool = Mining::ToolFromId(tool);
            if (r.tool == Tool::None) {
                error = where + ": no tool called '" + tool + "'";
                return false;
            }
        }
        if (const std::string armour = v.StringOr("armour", ""); !armour.empty()) {
            r.armour = Armour::FromId(armour);
            if (r.armour == ArmourPiece::Count) {
                error = where + ": no armour called '" + armour + "'";
                return false;
            }
        }
        const std::string upgrade = v.StringOr("upgrade", "");
        r.ironUpgrade = upgrade == "iron", r.steelUpgrade = upgrade == "steel";
        if (r.inputs.empty() || (makes.empty() && !r.MakesItem())) {
            error = where + ": it needs \"takes\" and something it makes";
            return false;
        }
        table.push_back(std::move(r));
    }
    Current().store(new std::vector<Recipe>(std::move(table)), std::memory_order_release); // (the old list stays: see Current)
    return true;
}

bool WriteDefaults(const std::string& path) {
    Json::Value list = Json::Array();
    for (const auto& r : Defaults()) {
        Json::Value v = Json::Object();
        Json::Member(v, "id") = Json::String(r.id);
        Json::Member(v, "name") = Json::String(r.name);
        Json::Member(v, "takes") = AmountsValue(r.inputs);
        if (r.output != BlockType::AIR) Json::Member(v, "makes") = AmountsValue({ { r.output, r.outputUnits } });
        if (r.tool != Tool::None) Json::Member(v, "tool") = Json::String(Mining::ToolId(r.tool));
        if (r.armour != ArmourPiece::Count) Json::Member(v, "armour") = Json::String(Armour::Id(r.armour));
        if (r.ironUpgrade || r.steelUpgrade) Json::Member(v, "upgrade") = Json::String(r.ironUpgrade ? "iron" : "steel");
        list.array.push_back(v);
    }
    return WriteFile(path,
                     "// Crafting recipes, in order (a crafter keeps its recipe by its place in this list,\n"
                     "// so add new ones at the end). Each takes some things and makes one: amounts in blocks\n"
                     "// (or items; 0.25 is a quarter). Instead of \"makes\": \"tool\" (pickaxe, axe, shovel,\n"
                     "// sword, bow, rod, chisel: made of stone), \"armour\" (cap, tunic, boots, shield, plate,\n"
                     "// tank), or \"upgrade\" (iron, steel: the tool in your hand). The game reads it again when\n"
                     "// it changes; a server and its players should share it.\n",
                     list);
}

} // namespace Crafting

namespace Smelting {

bool Load(const std::string& path, std::string& error) {
    Json::Value root;
    if (!Json::ParseFile(path, root, error)) return false;
    if (!root.IsArray()) {
        error = path + ": should be a [ ... ] list";
        return false;
    }
    std::vector<SmeltRecipe> table;
    for (size_t i = 0; i < root.array.size(); i++) {
        const std::string where = path + ": recipe " + std::to_string(i + 1);
        std::vector<Ingredient> takes, makes;
        if (!Amounts(root.array[i].Find("takes"), takes, where, error) || !Amounts(root.array[i].Find("makes"), makes, where, error)) return false;
        if (takes.empty() || takes.size() > 2 || makes.size() != 1) {
            error = where + ": it takes one or two things and makes one";
            return false;
        }
        SmeltRecipe r{ takes[0].type, takes[0].units, BlockType::AIR, 0, makes[0].type, makes[0].units };
        if (takes.size() == 2) r.extra = takes[1].type, r.extraUnits = takes[1].units;
        table.push_back(r);
    }
    Current().store(new std::vector<SmeltRecipe>(std::move(table)), std::memory_order_release);
    return true;
}

bool WriteDefaults(const std::string& path) {
    Json::Value list = Json::Array();
    for (const auto& r : Defaults()) {
        std::vector<Ingredient> takes{ { r.input, r.inputUnits } };
        if (r.extra != BlockType::AIR) takes.push_back({ r.extra, r.extraUnits });
        Json::Value v = Json::Object();
        Json::Member(v, "takes") = AmountsValue(takes);
        Json::Member(v, "makes") = AmountsValue({ { r.output, r.outputUnits } });
        list.array.push_back(v);
    }
    return WriteFile(path,
                     "// What smelters (and blast furnaces) make, in order: each does the first a chest can\n"
                     "// afford. Each takes one or two things and makes one; amounts in blocks (or items).\n"
                     "// The game reads it again when it changes.\n",
                     list);
}

} // namespace Smelting
