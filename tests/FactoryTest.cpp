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
//  - a drain takes water into fluid pipes, and an outlet lets it out elsewhere,
//  - a mining post hires dwarf miners only while it holds pay, and they dig the ore
//    in reach into it, paid a quarter of a block of food for each,
//  - multiblock structures: the hammer says what an unfinished one lacks and forms a
//    finished one; a blast furnace smelts fast from the chest at its item port;
//    breaking a part takes it apart; an oil derrick structure pumps crude.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "TestSections.h"
#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "world/Crafting.h"
#include "world/Creatures.h"
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

// In the chat, or in a machine's description (right-clicking it).
bool Has(const Client& c, const std::string& text) {
    for (const auto* lines : { &c.GetChat(), &c.GetInspections() })
        for (const auto& line : *lines)
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

int main(int argc, char** argv) {
    TestSections sections(argc, argv);
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

        // The server's report on a machine (none: not yet, or not a machine).
        auto machine = [&](const glm::ivec3& p) -> const net::MachineInfo* {
            for (const auto& m : a.client.GetMachines())
                if (m.pos == p) return &m;
            return nullptr;
        };

        if (sections.Run("machines")) {
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
            a.client.MoveChestItems(BlockType::STONE, 2, true);
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
            // An oil derrick structure over oil shale; its crude into a tank; a refinery
            // structure taking crude from that tank and putting fuel into another; an engine
            // on that, with an air network of its own and a gauge on it. The derrick and the
            // refinery share air.
            server.QueueConsoleCommand("tp Builder 33.5 0 27.5");
            pump(10.0, [&] { return a.feet.z < 28.0f; });
            const glm::ivec3 crude{ 33, 0, 31 }, fuel{ 37, 0, 31 }, engine{ 38, 0, 31 }, engineGauge{ 38, 2, 31 };
            fill({ 30, -12, 30 }, { 32, -10, 32 }, "oil_shale");
            // The derrick (pattern corner 30 0 30).
            fill({ 30, 0, 30 }, { 32, 0, 30 }, "steel_frame");
            fill({ 30, 0, 32 }, { 32, 0, 32 }, "steel_frame");
            fill({ 30, 0, 31 }, { 30, 0, 31 }, "air_pipe");
            fill({ 31, 0, 31 }, { 31, 0, 31 }, "oil_derrick");
            fill({ 32, 0, 31 }, { 32, 0, 31 }, "fluid_pipe");
            for (const glm::ivec3 c : { glm::ivec3(30, 1, 30), glm::ivec3(32, 1, 30), glm::ivec3(30, 1, 32), glm::ivec3(32, 1, 32) }) fill(c, c + glm::ivec3(0, 1, 0), "steel_frame");
            fill({ 31, 3, 30 }, { 31, 3, 32 }, "steel_frame");
            fill({ 30, 3, 31 }, { 32, 3, 31 }, "steel_frame");
            fill({ 31, 4, 31 }, { 31, 4, 31 }, "steel_frame");
            fill(crude, crude, "fluid_tank");
            // The refinery (pattern corner 34 0 30).
            server.QueueConsoleCommand("fill 34 0 30 36 2 32 machine_casing");
            for (const glm::ivec3 t : { glm::ivec3(34, 1, 30), glm::ivec3(36, 1, 30), glm::ivec3(34, 1, 32), glm::ivec3(36, 1, 32) }) fill(t, t, "fluid_tank");
            for (const glm::ivec3 c : { glm::ivec3(34, 2, 30), glm::ivec3(36, 2, 30), glm::ivec3(34, 2, 32), glm::ivec3(36, 2, 32) }) fill(c, c, "air");
            fill({ 35, 0, 30 }, { 35, 0, 30 }, "air_pipe");
            fill({ 34, 0, 31 }, { 34, 0, 31 }, "fluid_pipe");
            fill({ 35, 0, 31 }, { 35, 0, 31 }, "refinery");
            fill({ 36, 0, 31 }, { 36, 0, 31 }, "fluid_pipe");
            fill(fuel, fuel, "fluid_tank");
            fill(engine, engine, "engine");
            fill({ 38, 1, 31 }, { 38, 1, 31 }, "air_pipe");
            // Air to the derrick's and refinery's air pipes.
            fill({ 29, 0, 29 }, { 29, 0, 31 }, "air_pipe");
            fill({ 30, 0, 29 }, { 35, 0, 29 }, "air_pipe");
            a.client.PlaceBlock(engineGauge, BlockType::PRESSURE_GAUGE, 8, 2);
            Check(pump(10.0, [&] { return blockAt(engineGauge) == BlockType::PRESSURE_GAUGE && blockAt({ 35, 0, 31 }) == BlockType::REFINERY && blockAt({ 31, 4, 31 }) == BlockType::STEEL_FRAME; }),
                  "an oil derrick and a refinery built, tanks between them, an engine with its own air pipe and gauge");
            a.held = BlockType::HAMMER;
            wait(0.3);
            a.client.UseBed({ 31, 0, 31 });
            a.client.UseBed({ 35, 0, 31 });
            Check(pump(5.0, [&] { return blockAt({ 31, 0, 31 }) == BlockType::MULTIBLOCK && blockAt({ 35, 0, 31 }) == BlockType::MULTIBLOCK; }), "the hammer forms both");
            a.held = BlockType::AIR;
            bool crudeIn = false, fuelIn = false, engineRan = false;
            for (int i = 0; i < 30 && !(crudeIn && fuelIn && engineRan); i++) {
                server.QueueConsoleCommand("pressurise 29 0 31");
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
            const glm::ivec3 drain{ 40, 0, 40 }, cup{ 40, 1, 40 }, outlet{ 44, 0, 40 };
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

        }
        if (sections.Run("mining")) {
            std::cout << "Mining posts" << std::endl;
            // A mining post on the ground, with coal and iron ore buried within its reach.
            server.QueueConsoleCommand("tp Builder 50.5 0 47.5");
            // (Run on its own this is the first trip out here: wait for the ground to arrive.)
            pump(30.0, [&] { return a.feet.z < 48.0f && blockAt({ 50, -1, 50 }) != BlockType::COUNT && blockAt({ 54, -3, 50 }) != BlockType::COUNT; });
            const glm::ivec3 post{ 50, 0, 50 }, coal{ 54, -3, 50 }, iron{ 47, -2, 52 };
            fill(post, post, "mining_post");
            fill(coal, coal, "coal_ore");
            fill(iron, iron, "iron_ore");
            Check(pump(10.0, [&] { return blockAt(post) == BlockType::MINING_POST && blockAt(iron) == BlockType::IRON_ORE; }), "a mining post, with coal and iron ore buried near it");
            auto miners = [&] {
                int n = 0;
                for (const auto& cr : a.client.GetCreatures()) n += cr.type == CreatureType::DwarfMiner;
                return n;
            };
            wait(3.0);
            Check(miners() == 0 && machine(post) && machine(post)->state == static_cast<uint8_t>(Power::DrillState::NoPressure), "unpaid, no miners come");
            server.QueueConsoleCommand("give bread 2 Builder");
            pump(5.0, [&] { return a.client.GetResources(BlockType::BREAD) >= 2 * Crafting::kBlock; });
            a.client.OpenChest(post);
            Check(pump(10.0, [&] { return a.client.GetOpenChest() == post; }), "it opens like a chest");
            a.client.MoveChestItems(BlockType::BREAD, 1, true);
            Check(pump(10.0, [&] { return miners() >= 1; }), "paid in bread, dwarf miners come to work it");
            Check(pump(30.0, [&] { return a.client.GetChestUnits(BlockType::COAL) == Crafting::kBlock && a.client.GetChestUnits(BlockType::IRON_ORE) == Crafting::kBlock; }),
                  "they dig out the coal and the iron ore and put them in the post");
            Check(blockAt(coal) == BlockType::AIR && blockAt(iron) == BlockType::AIR, "the ore is gone from the ground");
            Check(a.client.GetChestUnits(BlockType::BREAD) == Crafting::kBlock - 2 * 16, "for a quarter of a block of bread each");
            a.client.BreakBlock(post);
            Check(pump(10.0, [&] { return miners() == 0; }), "breaking the post sends them away");

        }
        if (sections.Run("structures")) {
            std::cout << "Structures" << std::endl;
            // A blast furnace's pattern (turned as written: x and z as the pattern's), its
            // air pipe and item pipe out of the front, a chest of sand on the item pipe.
            server.QueueConsoleCommand("tp Builder 61.5 0 57.5");
            pump(30.0, [&] { return a.feet.z < 58.0f && blockAt({ 61, -1, 61 }) != BlockType::COUNT; }); // (and the ground there has arrived)
            server.QueueConsoleCommand("fill 60 0 60 62 2 62 fire_brick");
            fill({ 61, 0, 60 }, { 61, 0, 60 }, "air_pipe");
            fill({ 61, 0, 59 }, { 61, 0, 59 }, "air_pipe");
            fill({ 61, 1, 60 }, { 61, 1, 60 }, "item_pipe");
            fill({ 61, 1, 59 }, { 61, 1, 59 }, "chest");
            fill({ 61, 1, 61 }, { 61, 1, 61 }, "fire_brick"); // (the smelter's place, wrongly)
            pump(10.0, [&] { return blockAt({ 61, 1, 59 }) == BlockType::CHEST && blockAt({ 61, 1, 60 }) == BlockType::ITEM_PIPE; });
            a.held = BlockType::HAMMER;
            wait(0.3);
            a.client.UseBed({ 60, 0, 60 });
            Check(pump(5.0, [&] { return Has(a.client, "Not quite a Blast furnace: 1 block missing or wrong (at 61 1 61 it needs smelter)"); }),
                  "striking an unfinished blast furnace with the hammer says what's missing");
            fill({ 61, 1, 61 }, { 61, 1, 61 }, "smelter");
            pump(5.0, [&] { return blockAt({ 61, 1, 61 }) == BlockType::SMELTER; });
            // The fill also put a brick in its hollow top, over the smelter, which must be
            // left empty: it won't form with a stray block built into it.
            a.client.UseBed({ 62, 2, 62 });
            Check(pump(5.0, [&] { return Has(a.client, "Not quite a Blast furnace: 1 block in the way inside it (at 61 2 61), which must be left empty"); }),
                  "with a block in its hollow top it won't form, and says where the block in the way is");
            Check(blockAt({ 61, 1, 61 }) == BlockType::SMELTER, "and nothing was formed");
            fill({ 61, 2, 61 }, { 61, 2, 61 }, "air");
            pump(5.0, [&] { return blockAt({ 61, 2, 61 }) == BlockType::AIR; });
            a.client.UseBed({ 62, 2, 62 });
            Check(pump(5.0, [&] { return Has(a.client, "Formed a Blast furnace"); }), "finished, striking any of its blocks forms it");
            Check(pump(5.0, [&] { return blockAt({ 61, 1, 61 }) == BlockType::MULTIBLOCK && blockAt({ 60, 0, 60 }) == BlockType::MULTIBLOCK_PART &&
                                         blockAt({ 61, 0, 60 }) == BlockType::AIR_PIPE && blockAt({ 61, 1, 60 }) == BlockType::ITEM_PIPE; }),
                  "the smelter becomes its core and the bricks its parts; the pipes stay as they are (its ports)");
            a.held = BlockType::AIR;
            server.QueueConsoleCommand("give sand 4 Builder");
            pump(5.0, [&] { return a.client.GetResources(BlockType::SAND) >= 4 * Crafting::kBlock; });
            a.client.OpenChest({ 61, 1, 59 });
            pump(5.0, [&] { return a.client.GetOpenChest() == glm::ivec3(61, 1, 59); });
            a.client.MoveChestItems(BlockType::SAND, 4, true);
            const auto started = std::chrono::steady_clock::now();
            bool glass = false;
            for (int i = 0; i < 15 && !glass; i++) {
                server.QueueConsoleCommand("pressurise 61 0 59");
                glass = pump(1.0, [&] { return a.client.GetChestUnits(BlockType::GLASS) == 4 * Crafting::kBlock; });
            }
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            Check(glass && took < 6.0, "with air it smelts the chest's 4 sand into glass, fast (" + std::to_string(took).substr(0, 4) + " s; a smelter takes 8)");
            a.client.BreakBlock({ 60, 2, 62 });
            Check(pump(5.0, [&] { return blockAt({ 61, 1, 61 }) == BlockType::SMELTER && blockAt({ 60, 0, 60 }) == BlockType::FIRE_BRICK && blockAt({ 60, 2, 62 }) == BlockType::AIR; }),
                  "breaking a part takes it apart: every block back as it was (the broken one gone)");

            // An oil derrick: steel frames round a derrick block, over oil shale, its crude
            // port piped to a tank.
            server.QueueConsoleCommand("tp Builder 71.5 0 57.5");
            pump(10.0, [&] { return a.feet.x > 71.0f; });
            fill({ 70, -12, 60 }, { 72, -10, 62 }, "oil_shale");
            fill({ 70, 0, 60 }, { 72, 0, 60 }, "steel_frame");
            fill({ 70, 0, 62 }, { 72, 0, 62 }, "steel_frame");
            fill({ 70, 0, 61 }, { 70, 0, 61 }, "air_pipe");
            fill({ 69, 0, 61 }, { 69, 0, 61 }, "air_pipe");
            fill({ 71, 0, 61 }, { 71, 0, 61 }, "oil_derrick");
            fill({ 72, 0, 61 }, { 72, 0, 61 }, "fluid_pipe");
            fill({ 73, 0, 61 }, { 73, 0, 61 }, "fluid_tank");
            for (const glm::ivec3 c : { glm::ivec3(70, 1, 60), glm::ivec3(72, 1, 60), glm::ivec3(70, 1, 62), glm::ivec3(72, 1, 62) }) fill(c, c + glm::ivec3(0, 1, 0), "steel_frame");
            fill({ 71, 3, 60 }, { 71, 3, 62 }, "steel_frame");
            fill({ 70, 3, 61 }, { 72, 3, 61 }, "steel_frame");
            fill({ 71, 4, 61 }, { 71, 4, 61 }, "steel_frame");
            pump(10.0, [&] { return blockAt({ 71, 4, 61 }) == BlockType::STEEL_FRAME && blockAt({ 73, 0, 61 }) == BlockType::FLUID_TANK; });
            a.held = BlockType::HAMMER;
            wait(0.3);
            a.client.UseBed({ 71, 0, 61 });
            Check(pump(5.0, [&] { return Has(a.client, "Formed an Oil derrick"); }), "an oil derrick structure forms");
            a.held = BlockType::AIR;
            bool pumped = false;
            for (int i = 0; i < 10 && !pumped; i++) {
                server.QueueConsoleCommand("pressurise 69 0 61");
                pumped = pump(1.0, [&] {
                    const auto* t = machine({ 73, 0, 61 });
                    return t && t->depth == static_cast<uint8_t>(Fluids::Kind::Crude) && t->pressure > 0;
                });
            }
            Check(pumped, "it pumps crude oil from the shale under it into the tank");

            std::cout << "Crushing, coke and diesel" << std::endl;
            // A crusher on a chest of iron ore and stone, air piped to it.
            server.QueueConsoleCommand("tp Builder 81.5 0 51.5");
            pump(10.0, [&] { return a.feet.x > 81.0f; });
            fill({ 80, 0, 50 }, { 80, 0, 50 }, "chest");
            fill({ 80, 1, 50 }, { 80, 1, 50 }, "crusher");
            fill({ 80, 2, 50 }, { 80, 2, 50 }, "air_pipe");
            pump(10.0, [&] { return blockAt({ 80, 2, 50 }) == BlockType::AIR_PIPE && blockAt({ 80, 1, 50 }) == BlockType::CRUSHER; });
            server.QueueConsoleCommand("give iron_ore 1 Builder");
            server.QueueConsoleCommand("give stone 1 Builder");
            server.QueueConsoleCommand("give coal 4 Builder");
            pump(5.0, [&] { return a.client.GetResources(BlockType::IRON_ORE) >= Crafting::kBlock && a.client.GetResources(BlockType::COAL) >= 4 * Crafting::kBlock; });
            a.client.OpenChest({ 80, 0, 50 });
            pump(5.0, [&] { return a.client.GetOpenChest() == glm::ivec3(80, 0, 50); });
            a.client.MoveChestItems(BlockType::IRON_ORE, 1, true);
            a.client.MoveChestItems(BlockType::STONE, 1, true);
            bool crushed = false;
            for (int i = 0; i < 12 && !crushed; i++) {
                server.QueueConsoleCommand("pressurise 80 2 50");
                crushed = pump(1.0, [&] { return a.client.GetChestUnits(BlockType::CRUSHED_IRON) == 2 * Crafting::kBlock && a.client.GetChestUnits(BlockType::SAND) == Crafting::kBlock; });
            }
            Check(crushed && a.client.GetChestUnits(BlockType::IRON_ORE) == 0, "a crusher grinds an iron ore into two crushed iron, and stone into sand");

            // A coke oven: stone bricks round a smelter, a chimney on top, its item pipe to a
            // chest of coal and an air pipe.
            server.QueueConsoleCommand("fill 84 0 50 86 1 52 stone_brick");
            fill({ 85, 2, 51 }, { 85, 2, 51 }, "stone_brick");
            fill({ 84, 0, 51 }, { 84, 0, 51 }, "item_pipe");
            fill({ 83, 0, 51 }, { 83, 0, 51 }, "chest");
            fill({ 85, 0, 51 }, { 85, 0, 51 }, "smelter");
            fill({ 86, 0, 51 }, { 87, 0, 51 }, "air_pipe");
            pump(10.0, [&] { return blockAt({ 87, 0, 51 }) == BlockType::AIR_PIPE && blockAt({ 85, 0, 51 }) == BlockType::SMELTER && blockAt({ 83, 0, 51 }) == BlockType::CHEST; });
            a.held = BlockType::HAMMER;
            wait(0.3);
            a.client.UseBed({ 85, 1, 51 });
            Check(pump(5.0, [&] { return Has(a.client, "Formed a Coke oven"); }), "a coke oven forms");
            a.held = BlockType::AIR;
            a.client.OpenChest({ 83, 0, 51 });
            pump(5.0, [&] { return a.client.GetOpenChest() == glm::ivec3(83, 0, 51); });
            a.client.MoveChestItems(BlockType::COAL, 4, true);
            bool coke = false;
            for (int i = 0; i < 15 && !coke; i++) {
                server.QueueConsoleCommand("pressurise 87 0 51");
                coke = pump(1.0, [&] { return a.client.GetChestUnits(BlockType::COKE) == 2 * Crafting::kBlock; });
            }
            Check(coke && a.client.GetChestUnits(BlockType::COAL) == 0, "with air it bakes the 4 coal into 2 coke");

            // A diesel compressor: machine casings and tanks round an engine, its fuel port
            // piped to a tank of fuel, its air port to a pipe.
            server.QueueConsoleCommand("tp Builder 93.5 0 47.5");
            pump(10.0, [&] { return a.feet.x > 93.0f; });
            server.QueueConsoleCommand("fill 90 0 50 92 1 52 machine_casing");
            for (const glm::ivec3 c : { glm::ivec3(90, 1, 50), glm::ivec3(92, 1, 50), glm::ivec3(90, 1, 52), glm::ivec3(92, 1, 52) }) fill(c, c, "fluid_tank");
            fill({ 91, 0, 50 }, { 91, 0, 49 }, "air_pipe");
            fill({ 90, 0, 51 }, { 90, 0, 51 }, "fluid_pipe");
            fill({ 89, 0, 51 }, { 89, 0, 51 }, "fluid_tank");
            fill({ 91, 0, 51 }, { 91, 0, 51 }, "engine");
            pump(10.0, [&] { return blockAt({ 91, 0, 51 }) == BlockType::ENGINE && blockAt({ 89, 0, 51 }) == BlockType::FLUID_TANK && blockAt({ 92, 1, 52 }) == BlockType::FLUID_TANK; });
            a.held = BlockType::HAMMER;
            wait(0.3);
            a.client.UseBed({ 92, 0, 52 });
            Check(pump(5.0, [&] { return Has(a.client, "Formed a Diesel compressor"); }), "a diesel compressor forms");
            a.held = BlockType::AIR;
            a.client.Inspect({ 91, 0, 49 });
            Check(pump(5.0, [&] { return Has(a.client, "Air pressure 0%"); }), "without fuel its air pipes stay empty");
            server.QueueConsoleCommand("fluid 89 0 51 fuel");
            bool filled = false;
            for (int i = 0; i < 15 && !filled; i++) {
                pump(1.0, [] { return false; });
                a.client.Inspect({ 91, 0, 49 });
                filled = pump(1.0, [&] { return Has(a.client, "Air pressure 100%"); });
            }
            Check(filled, "with fuel it fills its air network");

            std::cout << "Steam" << std::endl;
            // A tank of water under a boiler, a tank over it for steam, a turbine beside that,
            // air pipes from the turbine, and a chest of coal by the boiler.
            server.QueueConsoleCommand("tp Builder 101.5 0 47.5");
            pump(10.0, [&] { return a.feet.x > 101.0f; });
            fill({ 100, 0, 50 }, { 100, 0, 50 }, "fluid_tank");
            fill({ 100, 1, 50 }, { 100, 1, 50 }, "boiler");
            fill({ 100, 2, 50 }, { 100, 2, 50 }, "fluid_tank");
            fill({ 101, 2, 50 }, { 101, 2, 50 }, "turbine");
            fill({ 102, 2, 50 }, { 103, 2, 50 }, "air_pipe");
            fill({ 99, 1, 50 }, { 99, 1, 50 }, "chest");
            pump(10.0, [&] { return blockAt({ 103, 2, 50 }) == BlockType::AIR_PIPE && blockAt({ 99, 1, 50 }) == BlockType::CHEST && blockAt({ 101, 2, 50 }) == BlockType::TURBINE; });
            server.QueueConsoleCommand("fluid 100 0 50 water");
            wait(1.0);
            a.client.Inspect({ 100, 1, 50 });
            Check(pump(5.0, [&] { return Has(a.client, "Boiler: out of fuel"); }), "with water but no coal, the boiler says it's out of fuel");
            server.QueueConsoleCommand("give coal 2 Builder");
            pump(5.0, [&] { return a.client.GetResources(BlockType::COAL) >= 2 * Crafting::kBlock; });
            a.client.OpenChest({ 99, 1, 50 });
            pump(5.0, [&] { return a.client.GetOpenChest() == glm::ivec3(99, 1, 50); });
            a.client.MoveChestItems(BlockType::COAL, 2, true);
            a.client.CloseChest();
            auto steamTank = [&] {
                for (const auto& m : a.client.GetMachines())
                    if (m.pos == glm::ivec3(100, 2, 50)) return m.depth == static_cast<uint8_t>(Fluids::Kind::Steam) && m.pressure > 0;
                return false;
            };
            bool turbineFilled = false;
            Check(pump(10.0, steamTank), "with coal, it boils the water into steam in the tank over it");
            for (int i = 0; i < 15 && !turbineFilled; i++) {
                pump(1.0, [] { return false; });
                a.client.Inspect({ 103, 2, 50 });
                turbineFilled = pump(1.0, [&] { return Has(a.client, "Air pressure 100%"); });
            }
            Check(turbineFilled, "and the turbine beside it fills its air pipes from the steam");

            std::cout << "Machines' own inventories" << std::endl;
            // A smelter with no chest at all: sand put into it is smelted there.
            server.QueueConsoleCommand("tp Builder 111.5 0 47.5");
            pump(10.0, [&] { return a.feet.x > 111.0f; });
            fill({ 110, 0, 50 }, { 110, 0, 50 }, "smelter");
            fill({ 111, 0, 50 }, { 112, 0, 50 }, "air_pipe");
            pump(10.0, [&] { return blockAt({ 110, 0, 50 }) == BlockType::SMELTER && blockAt({ 112, 0, 50 }) == BlockType::AIR_PIPE; });
            server.QueueConsoleCommand("give sand 2 Builder");
            pump(5.0, [&] { return a.client.GetResources(BlockType::SAND) >= 2 * Crafting::kBlock; });
            a.client.OpenChest({ 110, 0, 50 });
            Check(pump(5.0, [&] { return a.client.GetOpenChest() == glm::ivec3(110, 0, 50); }), "a smelter opens like a chest: its own inventory");
            a.client.MoveChestItems(BlockType::SAND, 2, true);
            bool ownGlass = false;
            for (int i = 0; i < 12 && !ownGlass; i++) {
                server.QueueConsoleCommand("pressurise 112 0 50");
                ownGlass = pump(1.0, [&] { return a.client.GetChestUnits(BlockType::GLASS) == 2 * Crafting::kBlock; });
            }
            Check(ownGlass, "with no chest beside it, it smelts the sand in its own inventory into glass, there");
            a.client.MoveChestItems(BlockType::GLASS, 2, false);
            Check(pump(5.0, [&] { return a.client.GetResources(BlockType::GLASS) >= 2 * Crafting::kBlock; }), "and the glass can be taken out of it");
            a.client.CloseChest();

        }
        a.client.Disconnect();
        Pump(a, 0.3, [] { return false; });
        server.Stop();
    }
    fs::remove_all(dir);
    if (!sections.Valid()) failures++;
    std::cout << (failures ? "FACTORY TEST FAILED (" : "ALL FACTORY CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
