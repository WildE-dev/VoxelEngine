// Air power over a real connection (world/Power.h). A creative player builds a mill in
// an empty world: a raised reservoir with two holes in its floor, each pouring water
// down one side of a water wheel; both wheels sit on one shaft, which turns a
// compressor; air pipes run from it to a drill (with a chest beside it) and a pump.
// Checks that
//  - wheels with no water flowing stand still, there's no air, and the drill and the
//    pump wait for pressure,
//  - something in a wheel's paddles jams it,
//  - water pouring down one side turns the wheels, both wheels on the shaft turn the
//    compressor, and the network fills with air,
//  - the drill digs down into the ground, and what it digs lands in the chest,
//  - the pump runs on the air,
//  - right-clicking parts describes them,
//  - cutting the air pipe starves the drill,
//  - stopping the water brings the wheels to a stop.
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
    Check(pump(20.0, [&] { return a.joined && blockAt({ 20, -1, 12 }) != BlockType::COUNT && blockAt({ 20, -1, 12 }) != BlockType::AIR; }), "a creative player joins");
    server.QueueConsoleCommand("tp Miller 15.5 0 11.5");
    pump(20.0, [&] { return a.feet.y < 0.5f; });
    int syncs = 0;
    auto sync = [&] {
        const std::string word = "sync" + std::to_string(++syncs);
        server.QueueConsoleCommand("msg Miller " + word);
        return pump(20.0, [&] { return Has(a.client, word); });
    };
    auto fill = [&](const glm::ivec3& lo, const glm::ivec3& hi, const char* block) {
        server.QueueConsoleCommand("fill " + std::to_string(lo.x) + " " + std::to_string(lo.y) + " " + std::to_string(lo.z) + " " + std::to_string(hi.x) + " " +
                                   std::to_string(hi.y) + " " + std::to_string(hi.z) + " " + block);
    };
    auto machine = [&](const glm::ivec3& pos, const std::function<bool(const net::MachineInfo&)>& test) {
        const auto* m = MachineAt(a.client, pos);
        return m && test(*m);
    };
    auto state = [](Power::DrillState s) { return static_cast<uint8_t>(s); };

    std::cout << "Building" << std::endl;
    // Two wheels on one shaft along z, a compressor at its end, air pipes on to a drill
    // and its chest, and a pump (with a trench of water to draw on) on a branch. (The
    // reservoir drains in a few seconds, but the air it leaves keeps the drill going.)
    const glm::ivec3 wheelA{ 20, 3, 10 }, wheelB{ 20, 3, 8 }, compressor{ 20, 3, 13 }, cut{ 20, 3, 14 }, drill{ 20, 3, 16 }, chest{ 21, 3, 16 };
    const glm::ivec3 pumpAt{ 19, 0, 16 };
    a.client.PlaceBlock(wheelA, BlockType::WATER_WHEEL, 8, 2);
    a.client.PlaceBlock(wheelB, BlockType::WATER_WHEEL, 8, 2);
    a.client.PlaceBlock({ 20, 3, 9 }, BlockType::SHAFT, 8, 2);
    a.client.PlaceBlock({ 20, 3, 11 }, BlockType::SHAFT, 8, 2);
    a.client.PlaceBlock({ 20, 3, 12 }, BlockType::SHAFT, 8, 2);
    fill(compressor, compressor, "compressor");
    fill({ 19, 3, 13 }, { 19, 3, 13 }, "valve");   // shut, with an air tank behind it
    fill({ 18, 3, 13 }, { 18, 3, 13 }, "air_tank");
    fill(cut, { 20, 3, 15 }, "air_pipe");
    fill(drill, drill, "drill");
    fill(chest, chest, "chest");
    fill({ 19, 1, 16 }, { 19, 3, 16 }, "air_pipe"); // down from beside the drill to the pump
    // The pump draws on a trench of water (3 blocks) and empties it into a closed tank.
    fill({ 17, 0, 13 }, { 17, 0, 19 }, "stone");
    fill({ 18, 0, 13 }, { 18, 0, 13 }, "stone");
    fill({ 19, 0, 13 }, { 19, 0, 15 }, "stone");
    fill({ 18, 0, 17 }, { 18, 0, 17 }, "stone");
    fill({ 20, 0, 17 }, { 20, 0, 19 }, "stone");
    fill({ 18, 0, 20 }, { 19, 0, 20 }, "stone");
    fill({ 19, 1, 17 }, { 19, 1, 17 }, "stone");
    fill({ 18, 1, 18 }, { 19, 1, 19 }, "stone"); // (the tank's lid)
    fill(pumpAt, pumpAt, "pump");
    fill({ 19, 0, 17 }, { 19, 0, 17 }, "pipe"); // the pump's water pipe, into the tank
    // The reservoir, empty for now: a stone tank above the wheels with two holes in its
    // floor, one over each wheel's +x side, and a pit under the wheels to catch it.
    fill({ 20, 5, 6 }, { 26, 15, 12 }, "stone");
    fill({ 21, 6, 7 }, { 25, 14, 11 }, "air");
    fill({ 21, 5, 10 }, { 21, 5, 10 }, "air");
    fill({ 21, 5, 8 }, { 21, 5, 8 }, "air");
    fill({ 19, -8, 6 }, { 23, -1, 12 }, "air");
    sync();
    Check(pump(20.0, [&] {
              Block w;
              return a.world.GetBlock(wheelA.x, wheelA.y, wheelA.z, w) && w.type == BlockType::WATER_WHEEL && w.axis == Block::kAlongZ &&
                     blockAt({ 20, 3, 12 }) == BlockType::SHAFT && blockAt(drill) == BlockType::DRILL && blockAt(pumpAt) == BlockType::PUMP &&
                     blockAt({ 21, 5, 10 }) == BlockType::AIR;
          }),
          "the mill is built (the wheels' axles along z, the way they were placed)");
    fill({ 18, 0, 14 }, { 18, 0, 16 }, "water");
    Check(pump(10.0, [&] { return machine(wheelA, [](auto& m) { return m.type == uint8_t(BlockType::WATER_WHEEL); }); }), "the server reports the wheel");
    wait(1.5);
    Check(machine(wheelA, [](auto& m) { return m.speed == 0; }) && machine(wheelB, [](auto& m) { return m.speed == 0; }), "with no water flowing the wheels stand still");
    Check(machine(compressor, [](auto& m) { return m.speed == 0 && m.depth == 2 && m.pressure == 0; }), "the compressor has both wheels on its shaft, and no air");
    Check(machine(drill, [&](auto& m) { return m.state == state(Power::DrillState::NoPressure); }), "the drill waits for pressure");
    Check(pump(10.0, [&] { return machine(pumpAt, [&](auto& m) { return m.state == state(Power::DrillState::NoPressure); }); }), "and so does the pump");

    fill({ 19, 4, 8 }, { 19, 4, 8 }, "stone");
    Check(pump(10.0, [&] { return machine(wheelB, [&](auto& m) { return m.state == state(Power::DrillState::Blocked); }); }), "a block in a wheel's paddles jams it");
    fill({ 19, 4, 8 }, { 19, 4, 8 }, "air");
    Check(pump(10.0, [&] { return machine(wheelB, [&](auto& m) { return m.state != state(Power::DrillState::Blocked); }); }), "and clearing it frees it");

    std::cout << "Running" << std::endl;
    fill({ 21, 6, 7 }, { 25, 14, 11 }, "water");
    Check(pump(20.0, [&] { return machine(wheelA, [](auto& m) { return m.speed > 64; }); }), "water pouring down one side turns a wheel");
    Check(pump(20.0, [&] { return machine(compressor, [](auto& m) { return m.speed > 0 && m.depth == 2; }) && machine(wheelB, [](auto& m) { return m.speed > 0; }); }),
          "both wheels on the shaft turn the compressor");
    Check(pump(20.0, [&] { return machine(compressor, [](auto& m) { return m.pressure > 40; }); }), "and the network fills with air");
    Check(pump(20.0, [&] { return machine(pumpAt, [&](auto& m) { return m.state == state(Power::DrillState::Working); }); }), "the pump runs on it");
    Check(pump(30.0, [&] { return blockAt({ 20, -1, 16 }) == BlockType::AIR && blockAt({ 20, -2, 16 }) == BlockType::AIR && blockAt({ 20, -3, 16 }) == BlockType::AIR; }),
          "the drill digs down into the ground");
    a.client.Inspect(compressor);
    Check(pump(10.0, [&] { return Has(a.client, "Compressor: wheels turning") && Has(a.client, "of 2 on its shafts") && Has(a.client, "Air pressure"); }), "right-clicking the compressor describes it");
    a.client.Inspect(wheelA);
    Check(pump(10.0, [&] { return Has(a.client, "Water wheel:") && Has(a.client, "It turns a compressor"); }), "and the wheel");

    server.QueueConsoleCommand("tp Miller 23.5 0 18.5");
    pump(10.0, [&] { return a.feet.x > 23.0f; });
    a.client.OpenChest(chest);
    Check(pump(20.0, [&] { return a.client.GetChestUnits(BlockType::STONE) >= 2 * Block::kUnitsPerBlock; }), "and what it digs is in the chest");
    a.client.CloseChest();

    std::cout << "Tanks and valves" << std::endl;
    const glm::ivec3 valve{ 19, 3, 13 }, tank{ 18, 3, 13 };
    a.client.Inspect(tank);
    Check(pump(10.0, [&] { return Has(a.client, "Air tank: holding 0 of 256"); }), "an air tank behind a shut valve gets no air");
    server.QueueConsoleCommand("tp Miller 15.5 0 11.5");
    pump(10.0, [&] { return a.feet.x < 16.0f; });
    a.client.UseBed(valve);
    Check(pump(10.0, [&] {
              Block b;
              return a.world.GetBlock(valve.x, valve.y, valve.z, b) && b.type == BlockType::VALVE_OPEN;
          }),
          "opening the valve");
    wait(2.0);
    a.client.Inspect(tank);
    Check(pump(10.0, [&] {
              for (const auto& line : a.client.GetChat())
                  if (line.text.find("Air tank: holding") != std::string::npos && line.text.find("holding 0 of") == std::string::npos &&
                      line.text.find("1 tanks") != std::string::npos)
                      return true;
              return false;
          }),
          "lets the network's air into the tank (which joins the network)");
    a.client.UseBed(valve);
    pump(10.0, [&] {
        Block b;
        return a.world.GetBlock(valve.x, valve.y, valve.z, b) && b.type == BlockType::VALVE;
    });

    std::cout << "Cutting the air" << std::endl;
    a.client.BreakBlock(cut);
    Check(pump(15.0, [&] { return machine(drill, [&](auto& m) { return m.state == state(Power::DrillState::NoPressure); }); }), "cutting the air pipe starves the drill");

    std::cout << "Stopping the water" << std::endl;
    fill({ 21, 5, 8 }, { 21, 5, 10 }, "stone"); // plug the holes
    Check(pump(30.0, [&] { return machine(wheelA, [](auto& m) { return m.speed == 0; }) && machine(wheelB, [](auto& m) { return m.speed == 0; }); }),
          "with the water stopped the wheels stop");

    a.client.Disconnect();
    Pump(a, 0.3, [] { return false; });
    server.Stop();
    std::cout << (failures ? "POWER TEST FAILED (" : "ALL POWER CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
