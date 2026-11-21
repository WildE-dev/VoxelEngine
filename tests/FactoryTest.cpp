// A small factory over a real connection (world/Power.h, world/Smelting.h). A creative
// player in an empty world sets up a drill over a column of sand, iron ore and coal ore, item
// pipes from it to a chest, a smelter on the chest, and air pipes to both. Checks that
//  - the drill sends what it digs down the item pipes into the chest,
//  - the smelter smelts it there (iron ore into twice the iron, sand into glass, iron
//    and coal into steel), lit
//    while it works,
//  - right-clicking the smelter describes it,
//  - an extractor sorts a chest's contents along item pipes, a filter pipe set (by
//    right-clicking it with glass in hand) to glass taking the glass and keeping the
//    rest out,
//  - a crafter, set the same way, makes stone bricks from stone in its chest,
//  - a harvester reaps ripe wheat into its chest and replants it,
//  - a pressure gauge fixed to an air pipe reads its network's pressure,
//  - an oil derrick pumps crude from oil shale into a tank, a refinery makes it fuel,
//    and an engine burns the fuel to fill its own air network,
//  - a drain takes water into fluid pipes, and an outlet lets it out elsewhere.
#include <algorithm>
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
#include "world/Fluids.h"
#include "world/Power.h"

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
    BlockType held = BlockType::AIR; // what they're holding (the server sees it)
    void Update() {
        client.Poll();
        if (!joined && client.IsWelcomed()) joined = true, feet = client.GetSpawn();
        if (auto c = client.TakeCorrection()) feet = *c;
        if (!joined) return;
        world.Update(feet + glm::vec3(0.0f, 1.62f, 0.0f));
        client.SendPlayerState(feet, glm::vec3(0.0f), 0.0f, 0.0f, net::kFlagOnGround, Tool::None, held);
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
        fill({ 12, -5, 8 }, { 12, -5, 8 }, "coal_ore");
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
            if (a.client.GetChestUnits(BlockType::STEEL_INGOT) >= 2 * Crafting::kBlock && a.client.GetChestUnits(BlockType::GLASS) >= 2 * Crafting::kBlock) break;
        }
        Check(sawSand, "the drill's sand comes down the item pipes into the chest");
        Check(a.client.GetChestUnits(BlockType::GLASS) == 2 * Crafting::kBlock && a.client.GetChestUnits(BlockType::SAND) == 0, "the smelter makes both blocks of sand into glass");
        Check(a.client.GetChestUnits(BlockType::IRON_ORE) == 0 && a.client.GetChestUnits(BlockType::IRON_INGOT) + a.client.GetChestUnits(BlockType::STEEL_INGOT) == 4 * Crafting::kBlock,
              "and both blocks of iron ore into 4 iron ingots");
        Check(a.client.GetChestUnits(BlockType::STEEL_INGOT) == 2 * Crafting::kBlock && a.client.GetChestUnits(BlockType::COAL) == 0,
              "and then, with the coal from the coal ore, 2 of them into steel");
        Check(sawLit, "lit while it works");
        Check(pump(6.0, [&] { return blockAt(smelter) == BlockType::SMELTER; }), "and dark again once there's nothing left to smelt");
        a.client.Inspect(smelter);
        Check(pump(5.0, [&] { return Has(a.client, "Smelter: nothing to smelt") && Has(a.client, "1 chest joined"); }), "right-clicking it says it has nothing to smelt, with 1 chest");


        std::cout << "Sorting" << std::endl;
        // An extractor on the chest (facing it, -x), pipes from its back to a chest A,
        // and a filter pipe set to glass branching off to a chest B; air from the
        // smelter's network.
        const glm::ivec3 extractor{ 16, 0, 8 }, chestA{ 19, 0, 8 }, filter{ 17, 0, 9 }, chestB{ 17, 0, 10 };
        server.QueueConsoleCommand("tp Builder 17.5 0 6.5");
        pump(10.0, [&] { return a.feet.z < 7.0f; });
        a.client.CloseChest();
        a.client.PlaceBlock(extractor, BlockType::EXTRACTOR, 8, 1);
        fill({ 17, 0, 8 }, { 18, 0, 8 }, "item_pipe");
        fill(chestA, chestA, "chest");
        fill(filter, filter, "filter_pipe");
        fill(chestB, chestB, "chest");
        fill({ 16, 1, 8 }, { 18, 1, 8 }, "air_pipe");
        Check(pump(10.0, [&] {
                  Block e;
                  return a.world.GetBlock(extractor.x, extractor.y, extractor.z, e) && e.type == BlockType::EXTRACTOR && e.axis == 1 && blockAt(chestB) == BlockType::CHEST;
              }),
              "an extractor facing the chest, pipes to chest A, and a filter pipe to chest B");
        a.held = BlockType::GLASS;
        wait(0.3);
        a.client.UseBed(filter);
        Check(pump(5.0, [&] { return Has(a.client, "lets only glass through"); }), "right-clicking the filter with glass in hand sets it to glass");
        Check(pump(5.0, [&] {
                  Block f;
                  return a.world.GetBlock(filter.x, filter.y, filter.z, f) && f.FilterType() == BlockType::GLASS;
              }),
              "(kept in the block)");
        a.held = BlockType::AIR;
        a.client.OpenChest(chestB);
        bool sorted = false;
        for (int i = 0; i < 20 && !sorted; i++) {
            server.QueueConsoleCommand("pressurise 12 1 8");
            sorted = pump(1.0, [&] { return a.client.GetChestUnits(BlockType::GLASS) == 2 * Crafting::kBlock; });
        }
        Check(sorted, "the glass goes through the filter to chest B");
        a.client.OpenChest(chestA);
        Check(pump(10.0, [&] {
                  return a.client.GetOpenChest() == chestA && a.client.GetChestUnits(BlockType::IRON_INGOT) == 2 * Crafting::kBlock &&
                         a.client.GetChestUnits(BlockType::STEEL_INGOT) == 2 * Crafting::kBlock && a.client.GetChestUnits(BlockType::GLASS) == 0;
              }),
              "the iron and steel to chest A (the filter keeps them out of B)");

        std::cout << "Crafting" << std::endl;
        // A crafter on chest A, set to stone bricks; stone put in the chest.
        const glm::ivec3 crafter{ 19, 1, 8 };
        fill(crafter, crafter, "crafter");
        pump(5.0, [&] { return blockAt(crafter) == BlockType::CRAFTER; });
        wait(1.0);
        a.client.Inspect(crafter);
        Check(pump(5.0, [&] { return Has(a.client, "Crafter: no recipe"); }), "a crafter with no recipe says so");
        a.held = BlockType::STONE_BRICK;
        wait(0.3);
        a.client.UseBed(crafter);
        Check(pump(5.0, [&] { return Has(a.client, "The crafter makes Stone bricks"); }), "right-clicking it with stone bricks in hand sets it to make them");
        a.held = BlockType::AIR;
        server.QueueConsoleCommand("give stone 2 Builder");
        pump(5.0, [&] { return a.client.GetResources(BlockType::STONE) >= 2 * Crafting::kBlock; });
        a.client.MoveChestItems(BlockType::STONE, 2 * Crafting::kBlock, true);
        pump(5.0, [&] { return a.client.GetChestUnits(BlockType::STONE) > 0; });
        // (Whatever went in: a block of stone makes a block of bricks.)
        bool crafted = false;
        uint32_t total = 0;
        for (int i = 0; i < 20 && !crafted; i++) {
            server.QueueConsoleCommand("pressurise 12 1 8");
            crafted = pump(1.0, [&] {
                total = std::max(total, a.client.GetChestUnits(BlockType::STONE) + a.client.GetChestUnits(BlockType::STONE_BRICK));
                return a.client.GetChestUnits(BlockType::STONE_BRICK) >= 2 * Crafting::kBlock;
            });
        }
        Check(crafted && a.client.GetChestUnits(BlockType::STONE) + a.client.GetChestUnits(BlockType::STONE_BRICK) == total,
              "it makes the stone in chest A into stone bricks, a block for a block (stone " + std::to_string(a.client.GetChestUnits(BlockType::STONE)) +
                  ", bricks " + std::to_string(a.client.GetChestUnits(BlockType::STONE_BRICK)) + ")");

        std::cout << "Harvesting" << std::endl;
        // A harvester beside chest A, with ripe wheat near it.
        const glm::ivec3 harvester{ 19, 0, 7 };
        fill(harvester, harvester, "harvester");
        fill({ 19, 1, 7 }, { 19, 1, 7 }, "air_pipe");
        fill({ 21, 0, 5 }, { 21, 0, 6 }, "ripe_wheat");
        pump(5.0, [&] { return blockAt({ 21, 0, 5 }) == BlockType::RIPE_WHEAT && blockAt(harvester) == BlockType::HARVESTER; });
        bool reaped = false;
        for (int i = 0; i < 20 && !reaped; i++) {
            server.QueueConsoleCommand("pressurise 12 1 8");
            reaped = pump(1.0, [&] { return a.client.GetChestUnits(BlockType::WHEAT) == 2 * Crafting::kBlock; });
        }
        Check(reaped, "the harvester reaps both crops of wheat into chest A");
        Check(pump(3.0, [&] { return blockAt({ 21, 0, 5 }) == BlockType::CROP && blockAt({ 21, 0, 6 }) == BlockType::CROP; }),
              std::string("and replants them (") + BlockId(blockAt({ 21, 0, 5 })) + ", " + BlockId(blockAt({ 21, 0, 6 })) + ")");

        std::cout << "Gauges" << std::endl;
        // A gauge fixed on top of an air pipe, and another on bare ground.
        const glm::ivec3 gauge{ 14, 2, 8 }, loose{ 21, 0, 9 };
        a.client.PlaceBlock(gauge, BlockType::PRESSURE_GAUGE, 8, 2);
        a.client.PlaceBlock(loose, BlockType::PRESSURE_GAUGE, 8, 2);
        auto reading = [&](const glm::ivec3& p) {
            for (const auto& m : a.client.GetMachines())
                if (m.pos == p && m.type == static_cast<uint8_t>(BlockType::PRESSURE_GAUGE)) return static_cast<int>(m.pressure);
            return -1;
        };
        server.QueueConsoleCommand("pressurise 12 1 8");
        Check(pump(5.0, [&] { return reading(gauge) > 200; }), "a gauge on an air pipe reads the network's pressure (" + std::to_string(reading(gauge)) + "/255)");
        Check(reading(loose) == 0, "one fixed to nothing reads nothing");
        a.client.Inspect(gauge);
        Check(pump(5.0, [&] { return Has(a.client, "Pressure gauge. Air pressure"); }), "right-clicking it describes the network");

        std::cout << "Oil" << std::endl;
        // Oil shale under a derrick; fluid pipes to a tank (crude); a refinery (facing
        // +x) from that tank into another (fuel); an engine on that, with an air
        // network of its own and a gauge on it. The derrick and refinery share air.
        server.QueueConsoleCommand("tp Builder 34.5 0 28.5");
        pump(10.0, [&] { return a.feet.z < 29.0f; });
        const glm::ivec3 derrick{ 31, 0, 31 }, crude{ 34, 0, 31 }, refinery{ 35, 0, 31 }, fuel{ 36, 0, 31 }, engine{ 37, 0, 31 }, engineGauge{ 37, 2, 31 };
        fill({ 30, -12, 30 }, { 32, -10, 32 }, "oil_shale");
        fill(derrick, derrick, "oil_derrick");
        fill({ 32, 0, 31 }, { 33, 0, 31 }, "fluid_pipe");
        fill(crude, crude, "fluid_tank");
        a.client.PlaceBlock(refinery, BlockType::REFINERY, 8, 0);
        fill(fuel, fuel, "fluid_tank");
        fill(engine, engine, "engine");
        fill({ 31, 1, 31 }, { 35, 1, 31 }, "air_pipe");
        fill({ 37, 1, 31 }, { 37, 1, 31 }, "air_pipe");
        a.client.PlaceBlock(engineGauge, BlockType::PRESSURE_GAUGE, 8, 2);
        Check(pump(10.0, [&] { return blockAt(engine) == BlockType::ENGINE && blockAt(refinery) == BlockType::REFINERY && blockAt(engineGauge) == BlockType::PRESSURE_GAUGE; }),
              "a derrick over oil shale, pipes to a crude tank, a refinery into a fuel tank, an engine with its own air pipe and gauge");
        auto machine = [&](const glm::ivec3& p) -> const net::MachineInfo* {
            for (const auto& m : a.client.GetMachines())
                if (m.pos == p) return &m;
            return nullptr;
        };
        bool crudeIn = false, fuelIn = false, engineRan = false;
        for (int i = 0; i < 30 && !(crudeIn && fuelIn && engineRan); i++) {
            server.QueueConsoleCommand("pressurise 31 1 31");
            pump(1.0, [&] {
                const auto* c = machine(crude);
                const auto* f = machine(fuel);
                const auto* e = machine(engine);
                if (c && c->depth == static_cast<uint8_t>(Fluids::Kind::Crude) && c->pressure > 0) crudeIn = true;
                if (f && f->depth == static_cast<uint8_t>(Fluids::Kind::Fuel) && f->pressure > 0) fuelIn = true;
                if (e && e->state == static_cast<uint8_t>(Power::DrillState::Working)) engineRan = true;
                return crudeIn && fuelIn && engineRan;
            });
        }
        Check(crudeIn, "the derrick pumps crude oil into the first tank");
        Check(fuelIn, "the refinery turns it into fuel in the second");
        Check(engineRan, "the engine runs on the fuel");
        const auto* g = machine(engineGauge);
        Check(pump(5.0, [&] { g = machine(engineGauge); return g && g->pressure > 0; }), "and fills its own air network (its gauge reads " + std::to_string(g ? g->pressure : 0) + "/255)");
        Block shale;
        bool drawn = false;
        for (int x = 30; x <= 32 && !drawn; x++)
            for (int z = 30; z <= 32 && !drawn; z++)
                if (a.world.GetBlock(x, -10, z, shale) && shale.type == BlockType::OIL_SHALE && shale.axis > 0) drawn = true;
        Check(drawn, "the shale it draws from records what's been taken");

        std::cout << "Drains and outlets" << std::endl;
        // A drain under a walled cup of water, a fluid pipe to a tank, a pipe on to an
        // outlet facing +x.
        server.QueueConsoleCommand("tp Builder 42.5 0 37.5");
        pump(10.0, [&] { return a.feet.z < 38.0f; });
        const glm::ivec3 drain{ 40, 0, 40 }, cup{ 40, 1, 40 }, outlet{ 44, 0, 40 }, out{ 45, 0, 40 };
        server.QueueConsoleCommand("fill 39 1 39 41 2 41 stone");
        fill(drain, drain, "drain");
        fill({ 41, 0, 40 }, { 41, 0, 40 }, "fluid_pipe");
        fill({ 42, 0, 40 }, { 42, 0, 40 }, "fluid_tank");
        fill({ 43, 0, 40 }, { 43, 0, 40 }, "fluid_pipe");
        a.client.PlaceBlock(outlet, BlockType::FLUID_OUTLET, 8, 0);
        fill({ 40, 2, 40 }, { 40, 2, 40 }, "air");
        fill(cup, cup, "water");
        Check(pump(10.0, [&] { return blockAt(outlet) == BlockType::FLUID_OUTLET; }), "a drain under a cup of water, piped through a tank to an outlet");
        auto waterAt = [&](const glm::ivec3& p) {
            Block b;
            return a.world.GetBlock(p.x, p.y, p.z, b) ? static_cast<int>(b.WaterVolume()) : -1;
        };
        Check(pump(10.0, [&] { return waterAt(cup) == 0; }), "the drain takes the water out of the cup");
        int around = 0;
        Check(pump(10.0, [&] {
                  around = 0;
                  for (int x = 45; x <= 52; x++)
                      for (int z = 33; z <= 47; z++) around += std::max(0, waterAt({ x, 0, z }));
                  return around > 64;
              }),
              "and it comes out of the outlet (" + std::to_string(around) + " units out there)");

        a.client.Disconnect();
        Pump(a, 0.3, [] { return false; });
        server.Stop();
    }
    fs::remove_all(dir);
    std::cout << (failures ? "FACTORY TEST FAILED (" : "ALL FACTORY CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
