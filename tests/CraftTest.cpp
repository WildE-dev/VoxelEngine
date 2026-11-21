// Crafting (survival). Checks that
//  - every recipe makes sense (takes something, makes something), and glass lets light
//    through and doesn't hide what's behind it,
//  - over a real connection: a recipe turns resources into its output, as many times
//    as asked; one that can't be afforded changes nothing; a tool is made once and
//    kept; creative players don't craft (they have everything),
//  - tools made are saved with the player.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <thread>

#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "world/Crafting.h"

namespace fs = std::filesystem;

// Test clients ask for a short view (chunks): plenty for the checks, and light enough
// to run every suite at once.
constexpr int kTestViewDistance = 4;

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

struct TestClient {
    ClientWorld world;
    Client client{ world };
    glm::vec3 feet{ 0.0f };
    bool joined = false;
    void Update() {
        client.Poll();
        if (!joined && client.IsWelcomed()) joined = true, feet = client.GetSpawn();
        if (auto c = client.TakeCorrection()) feet = *c;
        if (!joined) return;
        world.Update(feet + glm::vec3(0.0f, 1.62f, 0.0f));
        client.SendPlayerState(feet, glm::vec3(0.0f), 0.0f, 0.0f, net::kFlagOnGround);
    }
};

bool Pump(TestClient& c, double seconds, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        c.Update();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

Server::Options MakeOptions(const fs::path& dir) {
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = std::getenv("CRAFT_LOG") != nullptr;
    options.worldDirectory = dir.string();
    options.worldType = TerrainGenerator::Type::Empty;
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Survival;
    return options;
}

} // namespace

int main() {
    using Crafting::kBlock;
    std::cout << "Recipes" << std::endl;
    bool sensible = true;
    for (const auto& r : Crafting::Recipes()) {
        sensible &= !r.inputs.empty() && (r.MadeOnce() ? r.output == BlockType::AIR : r.outputUnits > 0 && r.output != BlockType::AIR);
        for (const auto& in : r.inputs) sensible &= in.units > 0 && in.type != BlockType::AIR;
    }
    Check(sensible && Crafting::Recipes().size() >= 10, "every recipe takes something and makes something");
    Check(Crafting::Find("planks") >= 0 && Crafting::Find("pickaxe") >= 0 && Crafting::Find("cake") < 0, "recipes are found by id");
    std::array<uint32_t, static_cast<size_t>(BlockType::COUNT)> held{};
    held[static_cast<size_t>(BlockType::LOG)] = 3 * kBlock;
    Check(Crafting::TimesAffordable(Crafting::Recipes()[Crafting::Find("planks")], held) == 3, "three logs make planks three times");
    Check(!Block(BlockType::GLASS).BlocksLight() && !Block(BlockType::GLASS).HidesNeighbours() && Block(BlockType::GLASS).IsFullBlock() &&
              Block(BlockType::STONE).HidesNeighbours(),
          "glass is solid but lets light through and shows what's behind it");

    std::random_device rd;
    const fs::path dir = fs::temp_directory_path() / ("voxel-craft-test-" + std::to_string(rd()));
    fs::remove_all(dir);
    {
        Server server;
        if (!server.Start(MakeOptions(dir))) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Crafter", error, kTestViewDistance);
        auto pump = [&](double s, const std::function<bool()>& done) { return Pump(a, s, done); };
        auto wait = [&](double s) { pump(s, [] { return false; }); };
        Check(pump(20.0, [&] { return a.joined; }), "a survival player joins");
        auto units = [&](BlockType t) { return a.client.GetResources(t); };
        // The client shows crafts at once (predicted); this waits until the server has
        // really done them: a command sent after them is answered after them (one
        // ordered channel), so once its reply is in, so are they.
        auto sync = [&] {
            auto replies = [&] {
                size_t n = 0;
                for (const auto& line : a.client.GetChat()) n += line.text.find("You're in") != std::string::npos;
                return n;
            };
            const size_t before = replies();
            a.client.SendChat("/gamemode");
            pump(20.0, [&] { return replies() > before; });
            wait(0.2); // (and its Resources answer)
        };
        Check(!a.client.HasTool(Tool::Pickaxe) && !a.client.CanCraft(Crafting::Find("planks")), "with nothing: no tools, nothing to craft");

        std::cout << "Crafting" << std::endl;
        server.QueueConsoleCommand("give log 2 Crafter");
        server.QueueConsoleCommand("give stone 3 Crafter");
        server.QueueConsoleCommand("give sand 1 Crafter");
        Check(pump(20.0, [&] { return units(BlockType::LOG) == 2 * kBlock && units(BlockType::STONE) == 3 * kBlock && units(BlockType::SAND) == kBlock; }),
              "given 2 logs, 3 stone and a sand");
        a.client.Craft(Crafting::Find("planks"), 2);
        sync();
        Check(pump(20.0, [&] { return units(BlockType::PLANKS) == 8 * kBlock && units(BlockType::LOG) == 0; }), "2 logs make 8 planks");
        a.client.Craft(Crafting::Find("planks"), 1); // (the client refuses; the server would too)
        wait(0.5);
        Check(units(BlockType::PLANKS) == 8 * kBlock, "with no logs left, no more planks");
        a.client.Craft(Crafting::Find("pickaxe"), 1);
        sync();
        Check(pump(20.0, [&] { return a.client.HasTool(Tool::Pickaxe); }) && units(BlockType::PLANKS) == 7 * kBlock &&
                  units(BlockType::STONE) == 3 * kBlock - kBlock * 3 / 2,
              "a pickaxe takes a plank and 1.5 stone, and is kept");
        Check(!a.client.CanCraft(Crafting::Find("pickaxe")), "a second pickaxe can't be made");
        a.client.Craft(Crafting::Find("glass"), 1);
        sync();
        Check(pump(20.0, [&] { return units(BlockType::GLASS) == kBlock && units(BlockType::SAND) == 0; }), "sand makes glass");
        // (Once the ground there has arrived: the server has it loaded too.)
        pump(20.0, [&] { Block b; return a.world.GetBlock(9, -1, 9, b) && !b.IsAir(); });
        a.client.PlaceBlock({ 9, 0, 9 }, BlockType::GLASS);
        sync();
        const bool built = pump(20.0, [&] { Block b; return a.world.GetBlock(9, 0, 9, b) && b.type == BlockType::GLASS; });
        Block at;
        a.world.GetBlock(9, 0, 9, at);
        Check(built && units(BlockType::GLASS) == 0, "and the glass can be built with (block " + std::string(BlockId(at.type)) + ", glass " +
                                                         std::to_string(units(BlockType::GLASS)) + ", sand " + std::to_string(units(BlockType::SAND)) + ")");
        a.client.Craft(Crafting::Find("lamp"), 1);
        sync();
        Check(units(BlockType::LAMP) == 0 && units(BlockType::PLANKS) == 7 * kBlock, "a lamp needs glass, which is used up: nothing happens");

        std::cout << "Swords and armour" << std::endl;
        server.QueueConsoleCommand("give stone 2 Crafter");
        server.QueueConsoleCommand("give hide 8 Crafter");
        sync();
        a.client.Craft(Crafting::Find("sword"), 1);
        sync();
        Check(pump(20.0, [&] { return a.client.HasTool(Tool::Sword); }) && units(BlockType::PLANKS) == 7 * kBlock - kBlock / 2,
              "half a plank and 2 stone make a sword");
        for (const char* piece : { "cap", "tunic", "boots" }) a.client.Craft(Crafting::Find(piece), 1);
        sync();
        Check(pump(20.0, [&] { return a.client.HasArmour(ArmourPiece::Cap) && a.client.HasArmour(ArmourPiece::Tunic) && a.client.HasArmour(ArmourPiece::Boots); }) &&
                  units(BlockType::HIDE) == 0,
              "8 hide make a cap, a tunic and boots, worn at once");
        Check(!a.client.CanCraft(Crafting::Find("cap")) && std::abs(Armour::Protection(a.client.GetArmour()) - 0.48f) < 1e-4f,
              "each is made once; together they take off 48%");
        Check(Armour::Reduce(7, a.client.GetArmour()) == 4 && Armour::Reduce(1, a.client.GetArmour()) == 1 && Armour::Reduce(4, 0) == 4,
              "a sword's 7 comes down to 4; a hit always does at least one");
        // (Planks were used for the sword: 6.5 left.)

        server.QueueConsoleCommand("gamemode creative Crafter");
        pump(20.0, [&] { return a.client.IsCreative(); });
        Check(a.client.HasTool(Tool::Axe) && !a.client.CanCraft(Crafting::Find("stone_brick")), "creative players have every tool and craft nothing");
        server.QueueConsoleCommand("gamemode survival Crafter");
        pump(20.0, [&] { return !a.client.IsCreative(); });
        Check(a.client.HasTool(Tool::Pickaxe) && !a.client.HasTool(Tool::Axe), "back in survival, only the pickaxe they made");
        a.client.Disconnect();
        Pump(a, 0.3, [] { return false; });
        server.Stop();
    }
    {
        std::cout << "Saving" << std::endl;
        Server server;
        if (!server.Start(MakeOptions(dir))) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Crafter", error, kTestViewDistance);
        const bool back = Pump(a, 20.0, [&] { return a.joined && a.client.GetResources(BlockType::PLANKS) > 0; });
        Check(back && a.client.HasTool(Tool::Pickaxe) && !a.client.HasTool(Tool::Shovel) && a.client.GetResources(BlockType::PLANKS) == 7 * kBlock - kBlock / 2,
              "after a restart: the pickaxe and the planks are still theirs (joined " + std::to_string(a.joined) + ", mode " +
                  std::to_string(static_cast<int>(a.client.GetMode())) + ", planks " + std::to_string(a.client.GetResources(BlockType::PLANKS)) +
                  ", pickaxe " + std::to_string(a.client.HasTool(Tool::Pickaxe)) + ")");
        Check(a.client.HasTool(Tool::Sword) && a.client.HasArmour(ArmourPiece::Tunic), "and so are the sword and the armour");
        a.client.Disconnect();
        Pump(a, 0.3, [] { return false; });
        server.Stop();
    }
    fs::remove_all(dir);
    std::cout << (failures ? "CRAFT TEST FAILED (" : "ALL CRAFT CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
