// Thin water, rain and the day/night clock. Checks that
//  - thin water follows its true volume, and water resting on water counts changes
//    down to a 32nd (so a lake's last layer drains evenly), while a thin film on dry
//    ground still ignores changes under an eighth,
//  - a lake's thin top layer isn't treated as a puddle that evaporates,
//  - rain tops up a partly drained pool to the brim and no further, and leaves alone
//    full water, puddles on dry ground and covered water,
//  - the server's clock runs, is saved with the world and resumes from it,
//  - a client's skip-time request jumps the clock, and every client gets the time and
//    the rain in its snapshots.
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
#include "server/ServerWorld.h"
#include "server/WorldStorage.h"

// Test clients ask for a short view (chunks): plenty for the checks, and light enough
// to run every suite at once.
constexpr int kTestViewDistance = 4;

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

bool WaitFor(double seconds, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

bool WaitLoaded(ServerWorld& world) {
    return WaitFor(60.0, [&] {
        for (int x = -1; x <= 2; x++)
            for (int y = -1; y <= 0; y++)
                for (int z = -1; z <= 2; z++) {
                    auto c = world.FindChunk({ x, y, z });
                    if (!c || !c->IsLoaded() || c->GetLightVersion() == 0) return false;
                }
        return true;
    });
}

int WaterAt(const ServerWorld& world, int x, int y, int z) {
    Block b;
    return world.GetBlock(x, y, z, b) ? b.WaterVolume() : -1;
}

bool Logged(ServerWorld& world, const glm::ivec3& pos) {
    std::vector<glm::ivec3> blocks, light;
    world.TakeChanges(blocks, light);
    for (const auto& b : blocks)
        if (b == pos) return true;
    return false;
}

// A stone basin sunk into flat ground: inside x, z in [x0+1, x0+3], rows -2 and -1.
void Basin(ServerWorld& world, int x0, int z0) {
    for (int x = x0; x <= x0 + 4; x++)
        for (int z = z0; z <= z0 + 4; z++) {
            world.SetBlock(x, -3, z, BlockType::STONE);
            for (int y = -2; y <= 0; y++)
                if (x == x0 || x == x0 + 4 || z == z0 || z == z0 + 4) world.SetBlock(x, y, z, BlockType::STONE);
                else if (y == 0) world.SetBlock(x, y, z, BlockType::AIR);
        }
}
void FillBasin(ServerWorld& world, int x0, int z0, int topVolume) {
    for (int x = x0 + 1; x <= x0 + 3; x++)
        for (int z = z0 + 1; z <= z0 + 3; z++) {
            world.SetBlock(x, -2, z, Block::Water(Block::kMaxWaterVolume));
            world.SetBlock(x, -1, z, topVolume ? Block::Water(static_cast<uint8_t>(topVolume)) : Block());
        }
}
int BasinTop(const ServerWorld& world, int x0, int z0) {
    int total = 0;
    for (int x = x0 + 1; x <= x0 + 3; x++)
        for (int z = z0 + 1; z <= z0 + 3; z++) total += std::max(0, WaterAt(world, x, -1, z));
    return total;
}

} // namespace

int main() {
    constexpr int kFull = Block::kMaxWaterVolume;

    std::cout << "Thin water" << std::endl;
    {
        Block deep = Block::Water(kFull);
        Check(deep.WithWater(120).shown == kFull, "small changes to deep water stay hidden (deadband)");
        Check(deep.WithWater(10).shown == 10 && Block::Water(14).WithWater(9).shown == 9, "thin water is drawn at its true volume");
        Block thin = Block::Water(6);
        Check(thin.WaterSurface32nds(false) == 4 && thin.WaterSurface32nds(true) == 2,
              "a thin film counts as an eighth on dry ground, its real height on water");

        TerrainGenerator terrain(TerrainGenerator::Type::Empty);
        ServerWorld world(terrain);
        world.SetWaterSimulationDistance(2);
        world.Update({ glm::ivec3(0) });
        Check(WaitLoaded(world), "chunks load");
        Basin(world, 0, 0);
        FillBasin(world, 0, 0, 12);
        world.SetBlock(10, 0, 10, Block::Water(12)); // a thin film on dry ground
        std::vector<glm::ivec3> scratch, light;
        world.TakeChanges(scratch, light);

        world.SetBlock(2, -1, 2, Block::Water(12).WithWater(8));
        Check(Logged(world, { 2, -1, 2 }), "a lake's top layer going from 12 to 8 units is sent to clients");
        world.SetBlock(10, 0, 10, Block::Water(12).WithWater(8));
        Check(!Logged(world, { 10, 0, 10 }), "the same change in a film on dry ground isn't");

        // Thin top layer of a pool: random ticks must not evaporate it.
        auto& sim = world.GetWater();
        for (int i = 0; i < 5; i++) sim.Tick(world);
        const int before = BasinTop(world, 0, 0);
        for (int i = 0; i < 6000; i++) sim.Tick(world);
        Check(BasinTop(world, 0, 0) == before && before > 0, "a pool's thin top layer doesn't evaporate");
    }

    std::cout << "Rain" << std::endl;
    {
        TerrainGenerator terrain(TerrainGenerator::Type::Empty);
        ServerWorld world(terrain);
        world.SetWaterSimulationDistance(2);
        world.Update({ glm::ivec3(0) });
        Check(WaitLoaded(world), "chunks load");
        Basin(world, 0, 0);      // half-drained pool, open to the sky
        FillBasin(world, 0, 0, kFull / 2);
        Basin(world, 8, 0);      // full pool: water level with the ground's surface row
        FillBasin(world, 8, 0, kFull);
        Basin(world, 0, 8);      // half-drained pool under a roof
        FillBasin(world, 0, 8, kFull / 2);
        for (int x = 0; x <= 4; x++)
            for (int z = 8; z <= 12; z++) world.SetBlock(x, 3, z, BlockType::STONE);
        world.SetBlock(20, 0, 20, Block::Water(10)); // a puddle on dry ground
        auto& sim = world.GetWater();
        for (int i = 0; i < 50; i++) sim.Tick(world);

        const int openBefore = BasinTop(world, 0, 0), coveredBefore = BasinTop(world, 0, 8);
        sim.SetRaining(true);
        for (int i = 0; i < 40000 && BasinTop(world, 0, 0) < 9 * kFull; i++) sim.Tick(world);
        for (int i = 0; i < 2000; i++) sim.Tick(world); // and some more once it's full
        Check(BasinTop(world, 0, 0) == 9 * kFull,
              "rain fills a half-drained pool to the brim (" + std::to_string(openBefore) + " -> " + std::to_string(BasinTop(world, 0, 0)) + " units)");
        bool above = false;
        for (int x = 0; x <= 12; x++)
            for (int z = 0; z <= 4; z++) above |= WaterAt(world, x, 0, z) > 0;
        Check(!above, "and no further: no water appears above full pools");
        Check(BasinTop(world, 8, 0) == 9 * kFull, "the full pool is unchanged");
        Check(BasinTop(world, 0, 8) == coveredBefore, "the covered pool gets nothing");
        Check(WaterAt(world, 20, 0, 20) == 10, "the puddle on dry ground neither grows nor dries up while it rains");
        Check(sim.GetRainedUnits() == static_cast<size_t>(9 * kFull - openBefore), "every unit rain added went into the open pool");
    }

    std::cout << "Clock" << std::endl;
    {
        std::random_device rd;
        const auto dir = std::filesystem::temp_directory_path() / ("voxel-weather-test-" + std::to_string(rd()));
        auto start = [&](float dayLength) {
            auto server = std::make_unique<Server>();
            Server::Options options;
            options.bindHost = "127.0.0.1";
            options.port = 0;
            options.viewDistance = kTestViewDistance;
            options.log = false;
            options.worldDirectory = dir.string();
            options.worldType = TerrainGenerator::Type::Empty;
            options.dayLengthSeconds = dayLength;
            options.weather = Server::Options::Weather::Rain;
            if (!server->Start(options)) server.reset();
            else server->RunInBackground();
            return server;
        };
        auto savedTime = [&] {
            WorldStorage storage(dir);
            return storage.Open() ? storage.GetTimeOfDay() : -1.0f;
        };

        auto server = start(20.0f); // 20-second days
        Check(server != nullptr, "server starts");
        if (!server) return EXIT_FAILURE;
        ClientWorld world;
        Client client(world);
        std::string error;
        Check(client.Connect("127.0.0.1", server->GetPort(), "Tester", error, kTestViewDistance), "a client connects");
        auto pump = [&](double seconds, const std::function<bool()>& done) {
            return WaitFor(seconds, [&] {
                client.Poll();
                return done();
            });
        };
        Check(pump(20.0, [&] { return client.IsWelcomed(); }), "and is welcomed");
        // A twentieth of the day takes a second of a 20 s day: wait for it, and check it
        // didn't come sooner (a loaded machine may make it later, never earlier).
        pump(20.0, [&] { return client.GetTimeOfDay() != 0.05f; }); // the first snapshot
        const float t0 = client.GetTimeOfDay();
        const auto c0 = std::chrono::steady_clock::now();
        const bool ran = pump(20.0, [&] { return client.GetTimeOfDay() >= t0 + 0.05f; });
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
        Check(ran && took > 0.8, "the clock runs (a twentieth of a 20 s day in " + std::to_string(took) + " s)");
        Check(pump(15.0, [&] { return client.GetRain() > 0.99f; }), "the forced rain reaches the client, faded in");
        // A thunderstorm: darker, with lightning near players (so this one stands
        // somewhere, telling the server where).
        server->QueueConsoleCommand("weather storm");
        std::vector<glm::ivec3> strikes;
        auto standing = [&](double seconds, const std::function<bool()>& done) {
            return WaitFor(seconds, [&] {
                client.Poll();
                client.SendPlayerState(client.GetSpawn(), glm::vec3(0.0f), 0.0f, 0.0f, net::kFlagOnGround);
                for (const auto& s : client.TakeLightning()) strikes.push_back(s);
                return done();
            });
        };
        Check(standing(20.0, [&] { return client.IsStorm(); }), "/weather storm makes it a thunderstorm, and the client knows");
        Check(standing(30.0, [&] { return !strikes.empty(); }), "lightning strikes near the player (" + std::to_string(strikes.size()) + ")");
        Check(!strikes.empty() && glm::distance(glm::vec2(strikes[0].x, strikes[0].z), glm::vec2(client.GetSpawn().x, client.GetSpawn().z)) < 42.0f,
              "within 40 blocks of them");
        auto said = [&](const std::string& text) {
            for (const auto& line : client.GetChat())
                if (line.text.find(text) != std::string::npos) return true;
            return false;
        };
        client.SendChat("/weather");
        Check(standing(10.0, [&] { return said("springs flowing at 250%"); }), "in a storm the springs flow two and a half times as hard");
        server->QueueConsoleCommand("weather drought");
        Check(standing(10.0, [&] { return said("a drought. The springs run low"); }), "/weather drought: no rain for days, and everyone's told it's a drought");
        client.SendChat("/weather");
        Check(standing(10.0, [&] { return said("springs flowing at 35%"); }), "and the springs slow to a third");
        server->QueueConsoleCommand("weather rain");
        Check(standing(10.0, [&] { return said("The drought breaks"); }), "rain breaks the drought");
        standing(20.0, [&] { return !client.IsStorm(); });

        // /time pause holds the day still (for operators: here, the console), and
        // /time resume lets it go on.
        client.SendChat("/time pause");
        Check(pump(10.0, [&] { return said("Only operators can do that"); }), "players can't stop the clock");
        server->QueueConsoleCommand("time pause");
        Check(pump(10.0, [&] { return said("paused the clock"); }), "/time pause stops it, and everyone's told");
        pump(0.5, [] { return false; });
        const float held = client.GetTimeOfDay();
        pump(1.5, [] { return false; });
        Check(client.GetTimeOfDay() == held, "the time of day stands still while it's paused");
        server->QueueConsoleCommand("time resume");
        Check(pump(10.0, [&] { return said("started the clock again") && client.GetTimeOfDay() != held; }), "/time resume starts it again");

        const float beforeStop = client.GetTimeOfDay();
        client.Disconnect();
        server->Stop();
        server.reset();
        const float saved = savedTime();
        Check(std::abs(saved - beforeStop) < 0.05f, "the time is saved with the world (" + std::to_string(saved) + ")");

        server = start(100000.0f); // the clock barely moves now
        Check(server != nullptr, "server restarts");
        server->Stop();
        server.reset();
        Check(std::abs(savedTime() - saved) < 0.01f, "and resumes from it");
        std::filesystem::remove_all(dir);
    }

    std::cout << (failures ? "WEATHER TEST FAILED (" : "ALL WEATHER CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
