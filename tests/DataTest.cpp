// The editable data files (Resources/Data). Checks that
//  - the JSON reader takes comments and trailing commas, and says which line an error
//    is on,
//  - a blocks file overrides just the details it gives (hardness, tool, light, name,
//    textures), leaving the rest as the game's own, and loading another puts them back,
//  - the shipped data folders (a file per block or model) read cleanly, each file writes
//    back the same, recipes.json and smelting.json match the game's own recipes, a
//    recipes file of one's own is used and a bad one refused; and blocks match
//    the game's own details.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "app/Settings.h"
#include "world/Block.h"
#include "world/BlockData.h"
#include "world/Smelting.h"
#include "world/Crafting.h"
#include "world/Json.h"
#include "world/Mining.h"

namespace fs = std::filesystem;

namespace {
int failures = 0;
void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}
} // namespace

int main() {
    std::cout << "JSON" << std::endl;
    {
        Json::Value v;
        std::string error;
        Json::Parser p("// a comment\n{ \"a\": [1, 2.5, -3,], \"b\": { \"c\": \"hi\" }, \"d\": true, }");
        Check(p.Parse(v, error) && v.Find("a")->array.size() == 3 && v.Find("a")->array[1].number == 2.5 && v.Find("b")->StringOr("c", "") == "hi" &&
                  v.BoolOr("d", false),
              "it reads objects, lists, numbers, strings and true, with comments and trailing commas");
        Json::Parser bad("{\n  \"a\": 1\n  \"b\": 2\n}");
        Check(!bad.Parse(v, error) && error.find("line 3") != std::string::npos, "a missing comma is reported on its line (" + error + ")");
    }

    std::cout << "Block details" << std::endl;
    const fs::path dir = fs::temp_directory_path() / "voxel_data_test";
    fs::create_directories(dir);
    {
        const fs::path file = dir / "blocks.json";
        std::ofstream(file) << "{ \"stone\": { \"hardness\": 9, \"tool\": \"axe\", \"name\": \"Rock\", \"light\": 7, \"textures\": { \"all\": \"dirt\" } } }";
        std::string error;
        Check(BlockData::Load(file.string(), error), "a blocks file loads");
        Check(Mining::Hardness(BlockType::STONE) == 9.0f && Mining::BestTool(BlockType::STONE) == Tool::Axe && GetLightEmission(BlockType::STONE) == 7,
              "stone takes its hardness, tool and light from it");
        Check(BlockData::NameOr(BlockType::STONE, "Stone") == std::string("Rock") && BlockData::Texture(BlockType::STONE, 3) == std::string("dirt"),
              "and its name and textures");
        Check(Mining::Hardness(BlockType::DIRT) == Mining::DefaultHardness(BlockType::DIRT) && !BlockData::Name(BlockType::DIRT),
              "blocks it doesn't mention keep the game's own");
        // Collision: boxes turned the way the block points, pipe arms where it joins,
        // another block's boxes taken over, and walked-through blocks.
        std::ofstream(file) << R"({
          "stone": { "collision": { "turns": true, "boxes": [{ "from": [0, 0, 0], "to": [1, 0.5, 0.25] }] } },
          "pipe": { "collision": { "boxes": [{ "from": [0.25, 0.25, 0.25], "to": [0.75, 0.75, 0.75] },
                                             { "from": [0.75, 0.25, 0.25], "to": [1, 0.75, 0.75], "joined": ["+x"] },
                                             { "from": [0, 0, 0], "to": [1, 0.1, 1], "joinedNone": ["+x"] },
                                             { "from": [0, 0.9, 0], "to": [1, 1, 1], "otherwise": true }] } },
          "air_pipe": { "collision": { "boxes": "pipe" } },
          "dirt": { "collision": { "solid": false } },
          "lever": { "collision": { "solid": true, "floor": false } }
        })";
        Check(BlockData::Load(file.string(), error), "a blocks file with collisions loads" + (error.empty() ? "" : " (" + error + ")"));
        glm::vec3 lo[Block::kMaxModelBoxes], hi[Block::kMaxModelBoxes];
        Block turned(BlockType::STONE);
        turned.axis = 0; // facing +x
        const int stoneBoxes = turned.ModelBoxes(0, lo, hi);
        Check(stoneBoxes == 1 && lo[0] == glm::vec3(0.0f, 0.0f, 0.0f) && hi[0] == glm::vec3(0.25f, 0.5f, 1.0f),
              "a block's boxes turn the way it faces (+z to +x)");
        const int joined = Block(BlockType::PIPE).ModelBoxes(1u << 0, lo, hi);
        const bool armAndRoof = joined == 3 && hi[1].x == 1.0f && lo[2].y == 0.9f;
        const int apart = Block(BlockType::AIR_PIPE).ModelBoxes(0, lo, hi);
        Check(armAndRoof && apart == 2 && hi[1].y == 0.1f, "boxes count by the faces joined, \"otherwise\" when the one before didn't, and are shared by id");
        Check(!Block(BlockType::DIRT).Collides() && Block(BlockType::STONE).Collides() && Block(BlockType::LEVER).Collides() &&
                  !Block(BlockType::LEVER).StoodOn() && Block(BlockType::DIRT).ModelBoxes(0, lo, hi) == -1,
              "\"solid\" and \"floor\" make a block walked through, or bumped into but not stood on");
        std::ofstream(file) << "{ }";
        Check(BlockData::Load(file.string(), error) && Mining::Hardness(BlockType::STONE) == Mining::DefaultHardness(BlockType::STONE) &&
                  GetLightEmission(BlockType::STONE) == 0,
              "loading another file puts them back");
    }
    fs::remove_all(dir);

    std::cout << "The shipped files" << std::endl;
    {
        std::string error;
        Check(BlockData::Load(BlockData::kPath, error), "Resources/Data/blocks (a file per block) reads cleanly" + (error.empty() ? "" : " (" + error + ")"));
        bool same = true;
        for (int i = 0; i < static_cast<int>(BlockType::COUNT); i++) {
            const BlockType t = static_cast<BlockType>(i);
            if (Mining::Hardness(t) != Mining::DefaultHardness(t) || Mining::BestTool(t) != Mining::DefaultBestTool(t) || GetLightEmission(t) != DefaultLightEmission(t)) {
                std::cout << "        " << (i == 0 ? "air" : BlockId(t)) << " differs" << std::endl;
                same = false;
            }
        }
        Check(same, "and matches the game's own hardness, tools and light for every block");
        Json::Value v;
        Check(Json::ParseDataPath("Resources/Data/creatures", v, error) && v.Find("dwarf") && v.Find("deer"),
              "Resources/Data/creatures (a file per model, named by its id) reads cleanly" + (error.empty() ? "" : " (" + error + ")"));
    }

    std::cout << "Writing models back (the model editor)" << std::endl;
    for (const char* path : { "Resources/Data/creatures", "Resources/Data/structures", "Resources/Data/blocks" }) {
        std::string error;
        bool same = true, commented = true;
        size_t files = 0;
        for (const auto& [id, file] : Json::DataFiles(path)) {
            Json::Value v, again;
            same = same && Json::ParseFile(file, v, error);
            const std::string text = Json::LeadingComments(file) + Json::Write(v);
            Json::Parser p(text);
            same = same && p.Parse(again, error) && Json::Write(again) == Json::Write(v);
            commented = commented && text.rfind("//", 0) == 0;
            files++;
        }
        Check(files > 0 && same, std::string(path) + ": each of its " + std::to_string(files) + " files written and read back is the same");
        Check(commented, "each with its comment kept at the top");
    }

    std::cout << "Recipes" << std::endl;
    {
        std::string error;
        const auto defaults = Crafting::Defaults();
        Check(Crafting::Load(Crafting::kRecipesPath, error), "Resources/Data/recipes.json reads cleanly" + (error.empty() ? "" : " (" + error + ")"));
        const auto& loaded = Crafting::Recipes();
        bool same = loaded.size() == defaults.size();
        for (size_t i = 0; same && i < loaded.size(); i++) {
            const Recipe &a = loaded[i], &b = defaults[i];
            same = a.id == b.id && a.name == b.name && a.output == b.output && a.outputUnits == b.outputUnits && a.tool == b.tool && a.armour == b.armour &&
                   a.ironUpgrade == b.ironUpgrade && a.steelUpgrade == b.steelUpgrade && a.inputs.size() == b.inputs.size();
            for (size_t k = 0; same && k < a.inputs.size(); k++) same = a.inputs[k].item == b.inputs[k].item && a.inputs[k].units == b.inputs[k].units;
            if (!same) std::cout << "        recipe " << i << " (" << b.id << ") differs" << std::endl;
        }
        Check(same, "and matches the game's own, recipe for recipe, in order (" + std::to_string(loaded.size()) + ")");
        Check(Smelting::Load(Smelting::kPath, error) && Smelting::Recipes().size() == Smelting::Defaults().size(),
              "Resources/Data/smelting.json reads cleanly, every recipe there" + (error.empty() ? "" : " (" + error + ")"));
        // A file of our own: a new recipe works, a mistake is refused (the old ones kept).
        const fs::path file = fs::temp_directory_path() / "voxel_recipes_test.json";
        {
            std::ofstream out(file);
            out << "// a test\n[ { \"id\": \"gravel_glass\", \"name\": \"Glass from sand\", \"takes\": { \"sand\": 0.5 }, \"makes\": { \"glass\": 2 } },\n"
                   "  { \"id\": \"pick\", \"takes\": { \"planks\": 1 }, \"tool\": \"pickaxe\" } ]\n";
        }
        Check(Crafting::Load(file.string(), error) && Crafting::Recipes().size() == 2 && Crafting::Find("gravel_glass") == 0 &&
                  Crafting::Recipes()[0].inputs[0].units == 32 && Crafting::Recipes()[0].outputUnits == 128 && Crafting::Recipes()[1].tool == Tool::Pickaxe,
              "a recipes file of its own is used: amounts in blocks (half a sand is 32 units), tools by name");
        {
            std::ofstream out(file);
            out << "[ { \"id\": \"bad\", \"takes\": { \"unobtainium\": 1 }, \"makes\": { \"glass\": 1 } } ]\n";
        }
        Check(!Crafting::Load(file.string(), error) && error.find("unobtainium") != std::string::npos && Crafting::Recipes().size() == 2,
              "one naming a block that isn't is refused, saying which (and the recipes stay as they were)");
        fs::remove(file);
        Crafting::Load(Crafting::kRecipesPath, error);
    }
    {
        Json::Value v = Json::Object();
        Json::Member(v, "name") = Json::String("say \"hi\"\\");
        Json::Member(v, "n") = Json::Number(0.1 + 0.2);
        Json::Value back;
        std::string error;
        Json::Parser p(Json::Write(v));
        Check(p.Parse(back, error) && back.StringOr("name", "") == "say \"hi\"\\" && Json::Write(v).find("0.3") != std::string::npos,
              "quotes are escaped and numbers written short");
    }

    std::cout << "Key bindings" << std::endl;
    {
        Settings saved;
        saved.keys[static_cast<size_t>(Action::Jump)] = GLFW_KEY_J;
        saved.padLook = 220.0f;
        const fs::path file = fs::temp_directory_path() / "voxel_settings_test.txt";
        saved.Save(file.string());
        Settings loaded;
        Check(loaded.Load(file.string()) && loaded.Key(Action::Jump) == GLFW_KEY_J && loaded.Key(Action::Forward) == GLFW_KEY_W && loaded.padLook == 220.0f,
              "a rebound key and the gamepad's look speed are saved and read back (the rest kept)");
        fs::remove(file);
    }

    std::cout << (failures ? "DATA TEST FAILED (" : "ALL DATA CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
