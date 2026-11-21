// Mining (survival). Checks the rules (world/Mining.h): how long each kind of block
// takes by hand, that the right tool is four times faster and the wrong one no
// faster, that carved blocks take less, and plants none; and over a real connection
// that the server
//  - refuses a break that wasn't mined, or wasn't mined for long enough,
//  - lets it through once it has been, by hand or faster with the right tool,
//  - holds the tool to what it was started with,
//  - picks pebbles up at once, as a quarter block of stone,
//  - and in creative breaks at once.
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "world/Mining.h"

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

bool Pump(std::initializer_list<TestClient*> clients, double seconds, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto* c : clients) c->Update();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

BlockType TypeAt(TestClient& c, const glm::ivec3& p) {
    Block b;
    return c.world.GetBlock(p.x, p.y, p.z, b) ? b.type : BlockType::COUNT;
}

bool Near(double a, double b) { return std::abs(a - b) < 1e-4; }

} // namespace

int main() {
    using namespace Mining;
    std::cout << "Rules" << std::endl;
    Check(Near(Seconds(Block(BlockType::DIRT), Tool::None), 0.6) && Near(Seconds(Block(BlockType::LOG), Tool::None), 2.0) &&
              Near(Seconds(Block(BlockType::STONE), Tool::None), 4.0) && Near(Seconds(Block(BlockType::STONE_BRICK), Tool::None), 5.0),
          "by hand: dirt 0.6 s, a log 2 s, stone 4 s, stone bricks 5 s");
    Check(Near(Seconds(Block(BlockType::STONE), Tool::Pickaxe), 1.0) && Near(Seconds(Block(BlockType::LOG), Tool::Axe), 0.5) &&
              Near(Seconds(Block(BlockType::DIRT), Tool::Shovel), 0.15),
          "the right tool is four times faster: pickaxe on stone, axe on logs, shovel on dirt");
    Check(Near(Seconds(Block(BlockType::STONE), Tool::Shovel), 4.0) && Near(Seconds(Block(BlockType::LOG), Tool::Pickaxe), 2.0),
          "the wrong tool is no better than a hand");
    Check(Seconds(Block::Placed(BlockType::TALL_GRASS), Tool::None) == 0.0f && Seconds(Block::Placed(BlockType::FLOWER_RED), Tool::None) == 0.0f,
          "plants break at once");
    Check(Near(Seconds(Block::Placed(BlockType::STONE_BRICK, 4), Tool::None), 2.5) &&
              Near(Seconds(Block::Placed(BlockType::STONE_BRICK, 2), Tool::None), 5.0 * kMinVolumeShare),
          "a half slab takes half as long; a thin one no less than 30%");
    bool everyType = true;
    for (int t = 1; t < static_cast<int>(BlockType::COUNT); t++) {
        const BlockType type = static_cast<BlockType>(t);
        if (type == BlockType::WATER || IsPlantType(type)) continue;
        everyType &= Hardness(type) > 0.0f;
    }
    Check(everyType, "every solid block takes some time");
    Check(ToolFromId("pickaxe") == Tool::Pickaxe && ToolFromId("axe") == Tool::Axe && ToolFromId("spoon") == Tool::None, "tools have ids");

    Server server;
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = std::getenv("MINING_LOG") != nullptr;
    options.worldType = TerrainGenerator::Type::Empty; // grass at row -1, dirt below, then stone
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Survival;
    if (!server.Start(options)) return EXIT_FAILURE;
    server.RunInBackground();
    TestClient a, builder;
    std::string error;
    a.client.Connect("127.0.0.1", server.GetPort(), "Miner", error, kTestViewDistance);
    builder.client.Connect("127.0.0.1", server.GetPort(), "Builder", error, kTestViewDistance);
    auto both = [&](double s, const std::function<bool()>& done) { return Pump({ &a, &builder }, s, done); };
    auto wait = [&](double s) { both(s, [] { return false; }); };
    Check(both(20.0, [&] { return a.joined && builder.joined && TypeAt(a, { 10, -1, 10 }) == BlockType::GRASS; }), "two players join");
    server.QueueConsoleCommand("gamemode creative Builder");
    both(20.0, [&] { return builder.client.IsCreative(); });
    // A row of stone to mine.
    for (int x = 10; x <= 15; x++) builder.client.PlaceBlock({ x, 0, 12 }, BlockType::STONE);
    Check(both(20.0, [&] { return TypeAt(a, { 15, 0, 12 }) == BlockType::STONE; }), "the builder (in creative) lays a row of stone");

    std::cout << "Survival" << std::endl;
    a.client.BreakBlock({ 10, 0, 12 });
    wait(1.0);
    Check(TypeAt(a, { 10, 0, 12 }) == BlockType::STONE && TypeAt(builder, { 10, 0, 12 }) == BlockType::STONE, "breaking without mining is refused");
    a.client.StartMining({ 10, 0, 12 }, Tool::None);
    wait(1.5);
    a.client.BreakBlock({ 10, 0, 12 });
    wait(1.0);
    Check(TypeAt(builder, { 10, 0, 12 }) == BlockType::STONE, "so is breaking stone after 1.5 s by hand (it takes 4)");
    a.client.StartMining({ 11, 0, 12 }, Tool::None);
    wait(4.1);
    a.client.BreakBlock({ 11, 0, 12 });
    Check(both(20.0, [&] { return TypeAt(builder, { 11, 0, 12 }) == BlockType::AIR; }), "after 4 s by hand it breaks");
    a.client.StartMining({ 12, 0, 12 }, Tool::Pickaxe);
    wait(1.1);
    a.client.BreakBlock({ 12, 0, 12 }, Tool::Pickaxe);
    wait(1.0);
    Check(TypeAt(builder, { 12, 0, 12 }) == BlockType::STONE, "a pickaxe not made yet is just a hand (no break in 1 s)");
    server.QueueConsoleCommand("give pickaxe Miner");
    server.QueueConsoleCommand("give shovel Miner");
    Check(both(20.0, [&] { return a.client.HasTool(Tool::Pickaxe) && a.client.HasTool(Tool::Shovel); }), "/give hands over tools");
    a.client.StartMining({ 12, 0, 12 }, Tool::Pickaxe);
    wait(1.1);
    a.client.BreakBlock({ 12, 0, 12 }, Tool::Pickaxe);
    Check(both(20.0, [&] { return TypeAt(builder, { 12, 0, 12 }) == BlockType::AIR; }), "a pickaxe of one's own breaks it in 1 s");
    a.client.StartMining({ 13, 0, 12 }, Tool::Shovel);
    wait(1.1);
    a.client.BreakBlock({ 13, 0, 12 }, Tool::Pickaxe);
    wait(1.0);
    Check(TypeAt(builder, { 13, 0, 12 }) == BlockType::STONE, "but not when it was started with a shovel (the tool must match)");
    a.client.StartMining({ 14, 0, 12 }, Tool::Pickaxe);
    wait(1.1);
    a.client.BreakBlock({ 15, 0, 12 }, Tool::Pickaxe);
    wait(1.0);
    Check(TypeAt(builder, { 15, 0, 12 }) == BlockType::STONE, "nor a block other than the one being mined");
    a.client.StartMining({ 8, -1, 8 }, Tool::Shovel);
    wait(0.3);
    a.client.BreakBlock({ 8, -1, 8 }, Tool::Shovel);
    Check(both(20.0, [&] { return TypeAt(builder, { 8, -1, 8 }) == BlockType::AIR; }) && a.client.GetResources(BlockType::GRASS) == 64,
          "a shovel takes grass in a moment, and the block's worth is collected");

    std::cout << "Pebbles" << std::endl;
    Check(Seconds(Block::Placed(BlockType::PEBBLES), Tool::None) == 0.0f && Block::Placed(BlockType::PEBBLES).ResourceUnits() == 16 &&
              Block::Placed(BlockType::PEBBLES).ResourceType() == BlockType::STONE,
          "pebbles are picked up at once, and are a quarter block of stone");
    builder.client.PlaceBlock({ 9, 0, 9 }, BlockType::PEBBLES);
    Check(both(20.0, [&] { return TypeAt(a, { 9, 0, 9 }) == BlockType::PEBBLES; }), "pebbles lie on the ground");
    const uint32_t stone = a.client.GetResources(BlockType::STONE);
    a.client.BreakBlock({ 9, 0, 9 });
    Check(both(20.0, [&] { return TypeAt(builder, { 9, 0, 9 }) == BlockType::AIR; }) &&
              both(20.0, [&] { return a.client.GetResources(BlockType::STONE) == stone + 16; }),
          "picking them up (no mining needed) adds 16 stone");
    a.client.PlaceBlock({ 9, 0, 9 }, BlockType::PEBBLES);
    Check(both(20.0, [&] { return TypeAt(builder, { 9, 0, 9 }) == BlockType::PEBBLES; }) && a.client.GetResources(BlockType::STONE) == stone,
          "and putting them down costs the 16 stone again");

    std::cout << "Creative" << std::endl;
    builder.client.BreakBlock({ 13, 0, 12 });
    Check(both(20.0, [&] { return TypeAt(a, { 13, 0, 12 }) == BlockType::AIR; }), "in creative a block breaks at once");

    a.client.Disconnect();
    builder.client.Disconnect();
    Pump({ &a, &builder }, 0.3, [] { return false; });
    server.Stop();
    std::cout << (failures ? "MINING TEST FAILED (" : "ALL MINING CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
