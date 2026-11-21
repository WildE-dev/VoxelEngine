// Air power over a real connection (world/Power.h). A creative player builds a little
// mill in an empty world: a trough of water with a pump over one end, whose pipe climbs
// and drops the water back in at the other end past a water wheel; air pipes run from
// the wheel to a drill with a chest beside it. Checks that
//  - a wheel with no water flowing past it stands still,
//  - the falling water turns the wheel, and the network fills with air,
//  - the drill digs down into the ground, and what it digs lands in the chest,
//  - right-clicking a part of the network describes it,
//  - without a chest the drill waits (and starts again when there's one),
//  - cutting the air pipe starves the drill,
//  - stopping the water (breaking the pump) brings the wheel to a stop.
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "world/Power.h"

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

const net::MachineInfo* MachineAt(const Client& c, const glm::ivec3& pos) {
    for (const auto& m : c.GetMachines())
        if (m.pos == pos) return &m;
    return nullptr;
}

bool Has(const Client& c, const std::string& text) {
    for (const auto& line : c.GetChat())
        if (line.text.find(text) != std::string::npos) return true;
    return false;
}

} // namespace

int main() {
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = std::getenv("POWER_LOG") != nullptr;
    options.worldType = TerrainGenerator::Type::Empty;
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Creative;
    options.creatures = false;
    Server server;
    if (!server.Start(options)) return EXIT_FAILURE;
    server.RunInBackground();

    TestClient a;
    std::string error;
    a.client.Connect("127.0.0.1", server.GetPort(), "Miller", error, kTestViewDistance);
    auto pump = [&](double s, const std::function<bool()>& done) { return Pump(a, s, done); };
    auto wait = [&](double s) { pump(s, [] { return false; }); };
    auto blockAt = [&](const glm::ivec3& p) {
        Block b;
        return a.world.GetBlock(p.x, p.y, p.z, b) ? b.type : BlockType::COUNT;
    };
    Check(pump(20.0, [&] { return a.joined && blockAt({ 13, -1, 9 }) != BlockType::COUNT && blockAt({ 13, -1, 9 }) != BlockType::AIR; }), "a creative player joins");
    server.QueueConsoleCommand("tp Miller 10.5 0 10.5");
    pump(20.0, [&] { return a.feet.y < 0.5f; });

    // Places each block and waits until the server has them all.
    auto build = [&](std::initializer_list<std::pair<glm::ivec3, BlockType>> blocks) {
        for (const auto& [p, t] : blocks) a.client.PlaceBlock(p, t);
        return pump(20.0, [&] {
            for (const auto& [p, t] : blocks)
                if (blockAt(p) != t) return false;
            return true;
        });
    };

    std::cout << "Building" << std::endl;
    const glm::ivec3 wheel{ 14, 1, 8 }, drill{ 14, 1, 11 }, chest{ 15, 1, 11 }, cut{ 14, 1, 10 }, pumpAt{ 12, 1, 7 };
    // The trough: x 12..14 at z 7, on the ground, walled in.
    Check(build({ { { 11, 0, 7 }, BlockType::STONE_BRICK }, { { 15, 0, 7 }, BlockType::STONE_BRICK },
                  { { 11, 0, 6 }, BlockType::STONE_BRICK }, { { 12, 0, 6 }, BlockType::STONE_BRICK }, { { 13, 0, 6 }, BlockType::STONE_BRICK },
                  { { 14, 0, 6 }, BlockType::STONE_BRICK }, { { 15, 0, 6 }, BlockType::STONE_BRICK },
                  { { 11, 0, 8 }, BlockType::STONE_BRICK }, { { 12, 0, 8 }, BlockType::STONE_BRICK }, { { 13, 0, 8 }, BlockType::STONE_BRICK },
                  { { 14, 0, 8 }, BlockType::STONE_BRICK }, { { 15, 0, 8 }, BlockType::STONE_BRICK } }),
          "a trough");
    // The wheel beside where the water will fall, air pipes to the drill, its chest.
    Check(build({ { wheel, BlockType::WATER_WHEEL }, { { 14, 1, 9 }, BlockType::AIR_PIPE }, { cut, BlockType::AIR_PIPE },
                  { drill, BlockType::DRILL }, { chest, BlockType::CHEST } }),
          "a wheel, air pipes, a drill and a chest");
    Check(pump(10.0, [&] { const auto* w = MachineAt(a.client, wheel); return w && w->type == uint8_t(BlockType::WATER_WHEEL); }),
          "the server reports the wheel");
    wait(1.0);
    {
        const auto* w = MachineAt(a.client, wheel);
        const auto* d = MachineAt(a.client, drill);
        Check(w && w->speed == 0 && w->pressure == 0, "with no water flowing it stands still, and there's no air");
        Check(d && d->state == uint8_t(Power::DrillState::NoPressure), "and the drill waits for pressure");
    }

    // Water in the trough, the pump over its far end, and its pipe up and over to drop
    // the water past the wheel (closed in, so it only comes out downwards).
    Check(build({ { { 12, 0, 7 }, BlockType::WATER }, { { 13, 0, 7 }, BlockType::WATER },
                  { { 15, 3, 7 }, BlockType::STONE }, { { 14, 4, 7 }, BlockType::STONE }, { { 14, 3, 6 }, BlockType::STONE }, { { 14, 3, 8 }, BlockType::STONE },
                  { pumpAt, BlockType::PUMP }, { { 12, 2, 7 }, BlockType::PIPE }, { { 12, 3, 7 }, BlockType::PIPE }, { { 13, 3, 7 }, BlockType::PIPE },
                  { { 14, 3, 7 }, BlockType::PIPE } }),
          "water in the trough, and a pump to lift it");

    std::cout << "Running" << std::endl;
    Check(pump(20.0, [&] { const auto* w = MachineAt(a.client, wheel); return w && w->speed > 128; }), "the falling water turns the wheel (over half speed)");
    Check(pump(20.0, [&] { const auto* w = MachineAt(a.client, wheel); return w && w->pressure > 40; }), "and the network fills with air");
    Check(pump(20.0, [&] { const auto* d = MachineAt(a.client, drill); return d && d->state == uint8_t(Power::DrillState::Working); }), "the drill starts");
    Check(pump(20.0, [&] { return blockAt({ 14, -1, 11 }) == BlockType::AIR && blockAt({ 14, -2, 11 }) == BlockType::AIR && blockAt({ 14, -3, 11 }) == BlockType::AIR; }), "it digs down into the ground");
    a.client.OpenChest(chest);
    Check(pump(20.0, [&] { return a.client.GetChestUnits(BlockType::STONE) >= 2 * Block::kUnitsPerBlock; }), "and what it digs is in the chest");
    a.client.CloseChest();
    {
        const auto* d = MachineAt(a.client, drill);
        Check(d && d->depth >= 3, "it reports how deep it is");
    }

    a.client.Inspect(wheel);
    Check(pump(10.0, [&] { return Has(a.client, "Water wheel: turning") && Has(a.client, "Air pressure"); }), "right-clicking the wheel describes it and its network");
    a.client.Inspect(cut);
    Check(pump(10.0, [&] { return Has(a.client, "drills working: 1 of 1"); }), "and a pipe describes the network");

    std::cout << "No chest" << std::endl;
    a.client.BreakBlock(chest);
    Check(pump(10.0, [&] { const auto* d = MachineAt(a.client, drill); return d && d->state == uint8_t(Power::DrillState::NoChest); }), "without a chest the drill waits");
    Check(build({ { chest, BlockType::CHEST } }) &&
              pump(10.0, [&] { const auto* d = MachineAt(a.client, drill); return d && d->state == uint8_t(Power::DrillState::Working); }),
          "and starts again with a new one");

    std::cout << "Cutting the air" << std::endl;
    a.client.BreakBlock(cut);
    Check(pump(15.0, [&] { const auto* d = MachineAt(a.client, drill); return d && d->state == uint8_t(Power::DrillState::NoPressure); }), "cutting the air pipe starves the drill");
    {
        const auto* w = MachineAt(a.client, wheel);
        Check(w && w->pressure > 64, "while the wheel's side keeps its pressure");
    }

    std::cout << "Stopping the water" << std::endl;
    a.client.BreakBlock(pumpAt);
    Check(pump(30.0, [&] { const auto* w = MachineAt(a.client, wheel); return w && w->speed == 0; }), "with the pump gone the water settles and the wheel stops");

    a.client.Disconnect();
    Pump(a, 0.3, [] { return false; });
    server.Stop();
    std::cout << (failures ? "POWER TEST FAILED (" : "ALL POWER CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
