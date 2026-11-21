// The editable data files (Resources/Data). Checks that
//  - the JSON reader takes comments and trailing commas, and says which line an error
//    is on,
//  - a blocks file overrides just the details it gives (hardness, tool, light, name,
//    textures), leaving the rest as the game's own, and loading another puts them back,
//  - the shipped blocks.json and creatures.json read cleanly, and blocks.json matches
//    the game's own details.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "world/BlockData.h"
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
        std::ofstream(file) << "{ }";
        Check(BlockData::Load(file.string(), error) && Mining::Hardness(BlockType::STONE) == Mining::DefaultHardness(BlockType::STONE) &&
                  GetLightEmission(BlockType::STONE) == 0,
              "loading another file puts them back");
    }
    fs::remove_all(dir);

    std::cout << "The shipped files" << std::endl;
    {
        std::string error;
        Check(BlockData::Load(BlockData::kPath, error), "Resources/Data/blocks.json reads cleanly" + (error.empty() ? "" : " (" + error + ")"));
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
        Check(Json::ParseFile("Resources/Data/creatures.json", v, error) && v.Find("dwarf") && v.Find("deer"),
              "Resources/Data/creatures.json reads cleanly" + (error.empty() ? "" : " (" + error + ")"));
    }

    std::cout << (failures ? "DATA TEST FAILED (" : "ALL DATA CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
