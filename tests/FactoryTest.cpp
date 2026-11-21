// A small factory over a real connection (world/Power.h, world/Smelting.h). A creative
// player in an empty world sets up a drill over a column of sand and iron ore, item
// pipes from it to a chest, a smelter on the chest, and air pipes to both. Checks that
//  - the drill sends what it digs down the item pipes into the chest,
//  - the smelter smelts it there (iron ore into twice the iron, sand into glass), lit
//    while it works,
//  - right-clicking the smelter describes it.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "world/Crafting.h"

namespace fs = std::filesystem;

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

bool Has(const Client& c, const std::string& text) {
    for (const auto& line : c.GetChat())
        if (line.text.find(text) != std::string::npos) return true;
    return false;
}

Server::Options MakeOptions(const fs::path& dir) {
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = std::getenv("FACTORY_LOG") != nullptr;
    options.worldDirectory = dir.string();
    options.worldType = TerrainGenerator::Type::Empty;
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Creative;
    options.creatures = false;
    return options;
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / ("voxel_factory_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        Server server;
        if (!server.Start(MakeOptions(dir))) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Builder", error, kTestViewDistance);
        auto pump = [&](double s, const std::function<bool()>& done) { return Pump(a, s, done); };
        auto wait = [&](double s) { pump(s, [] { return false; }); };
        auto blockAt = [&](const glm::ivec3& p) {
            Block b;
            return a.world.GetBlock(p.x, p.y, p.z, b) ? b.type : BlockType::COUNT;
        };
        auto fill = [&](const glm::ivec3& lo, const glm::ivec3& hi, const char* block) {
            server.QueueConsoleCommand("fill " + std::to_string(lo.x) + " " + std::to_string(lo.y) + " " + std::to_string(lo.z) + " " + std::to_string(hi.x) + " " +
                                       std::to_string(hi.y) + " " + std::to_string(hi.z) + " " + block);
        };
        Check(pump(20.0, [&] { return a.joined && blockAt({ 8, -1, 8 }) != BlockType::COUNT && blockAt({ 8, -1, 8 }) != BlockType::AIR; }), "a creative player joins");
        server.QueueConsoleCommand("tp Builder 10.5 0 10.5");
        pump(20.0, [&] { return a.feet.z > 10.0f; });

        std::cout << "Building" << std::endl;
        const glm::ivec3 drill{ 12, 0, 8 }, chest{ 15, 0, 8 }, smelter{ 15, 1, 8 };
        fill({ 12, -4, 8 }, { 12, -3, 8 }, "iron_ore");
        fill({ 12, -2, 8 }, { 12, -1, 8 }, "sand");
        fill(drill, drill, "drill");
        fill({ 13, 0, 8 }, { 14, 0, 8 }, "item_pipe");
        fill(chest, chest, "chest");
        fill(smelter, smelter, "smelter");
        fill({ 12, 1, 8 }, { 14, 1, 8 }, "air_pipe");
        Check(pump(10.0, [&] { return blockAt(smelter) == BlockType::SMELTER && blockAt({ 14, 1, 8 }) == BlockType::AIR_PIPE && blockAt({ 12, -4, 8 }) == BlockType::IRON_ORE; }),
              "a drill over sand and iron ore, item pipes to a chest, a smelter on the chest, air pipes to both");
        wait(1.0); // (an air update or two)
        a.client.Inspect(smelter);
        Check(pump(5.0, [&] { return Has(a.client, "Smelter: waiting for air pressure"); }), "with no air, the smelter waits for pressure");
        a.client.OpenChest(chest);
        Check(pump(10.0, [&] { return a.client.GetOpenChest() == chest; }), "the chest opens");

        std::cout << "Working" << std::endl;
        bool sawSand = false, sawLit = false;
        for (int i = 0; i < 40; i++) {
            server.QueueConsoleCommand("pressurise 12 1 8");
            pump(1.0, [&] {
                if (a.client.GetChestUnits(BlockType::SAND) > 0) sawSand = true;
                if (blockAt(smelter) == BlockType::SMELTER_LIT) sawLit = true;
                return false;
            });
            if (a.client.GetChestUnits(BlockType::IRON_INGOT) >= 4 * Crafting::kBlock && a.client.GetChestUnits(BlockType::GLASS) >= 2 * Crafting::kBlock) break;
        }
        Check(sawSand, "the drill's sand comes down the item pipes into the chest");
        Check(a.client.GetChestUnits(BlockType::GLASS) == 2 * Crafting::kBlock && a.client.GetChestUnits(BlockType::SAND) == 0, "the smelter makes both blocks of sand into glass");
        Check(a.client.GetChestUnits(BlockType::IRON_INGOT) == 4 * Crafting::kBlock && a.client.GetChestUnits(BlockType::IRON_ORE) == 0,
              "and both blocks of iron ore into 4 iron ingots");
        Check(sawLit, "lit while it works");
        Check(pump(6.0, [&] { return blockAt(smelter) == BlockType::SMELTER; }), "and dark again once there's nothing left to smelt");
        a.client.Inspect(smelter);
        Check(pump(5.0, [&] { return Has(a.client, "Smelter: nothing to smelt") && Has(a.client, "1 chest joined"); }), "right-clicking it says it has nothing to smelt, with 1 chest");

        a.client.Disconnect();
        Pump(a, 0.3, [] { return false; });
        server.Stop();
    }
    fs::remove_all(dir);
    std::cout << (failures ? "FACTORY TEST FAILED (" : "ALL FACTORY CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
