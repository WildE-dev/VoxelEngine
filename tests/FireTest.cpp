// Fire (world/Fire.h), over a real connection. Checks that
//  - fire spreads through a little wood (trees of logs and leaves) and burns it down;
//  - fire on bare stone, with nothing to burn, dies out in a few seconds;
//  - stone and obsidian don't burn;
//  - water flowing in puts it out, and so does a block put where it is (smothering);
//  - a bucket scoops up a block of water, and poured on a fire puts out the fires round it;
//  - a survival player standing in fire is burned, and goes on burning after stepping
//    out, until they get into water;
//  - items lying in fire are lost;
//  - a sprinkler on a tank of water puts out the fires round and below it by itself;
//    one with a channel waits for it, and a heat sensor on that channel, seeing fire,
//    sets it off.
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
#include "world/TerrainGenerator.h"

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

} // namespace

int main() {
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.worldType = TerrainGenerator::Type::Empty; // grass at row -1
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Creative;
    options.creatures = false;
    Server server;
    if (!server.Start(options)) return EXIT_FAILURE;
    server.RunInBackground();
    TestClient a;
    std::string error;
    a.client.Connect("127.0.0.1", server.GetPort(), "Warden", error, kTestViewDistance);
    auto at = [&](const glm::ivec3& p) {
        Block b;
        return a.world.GetBlock(p.x, p.y, p.z, b) ? b.type : BlockType::COUNT;
    };
    auto fill = [&](const glm::ivec3& lo, const glm::ivec3& hi, const char* block) {
        server.QueueConsoleCommand("fill " + std::to_string(lo.x) + " " + std::to_string(lo.y) + " " + std::to_string(lo.z) + " " + std::to_string(hi.x) + " " +
                                   std::to_string(hi.y) + " " + std::to_string(hi.z) + " " + block);
    };
    auto count = [&](const glm::ivec3& lo, const glm::ivec3& hi, BlockType type) {
        int n = 0;
        for (int x = lo.x; x <= hi.x; x++)
            for (int y = lo.y; y <= hi.y; y++)
                for (int z = lo.z; z <= hi.z; z++) n += at({ x, y, z }) == type ? 1 : 0;
        return n;
    };
    Check(Pump(a, 20.0, [&] { return a.joined && at({ 10, -1, 10 }) == BlockType::GRASS; }), "a creative player joins");
    server.QueueConsoleCommand("tp Warden 8.5 0 8.5");
    Pump(a, 5.0, [&] { return std::abs(a.feet.x - 8.5f) < 0.1f; });

    std::cout << "A wood on fire" << std::endl;
    {
        // Four trees close together (trunks four high, a crown of leaves), on a stone floor.
        const glm::ivec3 lo{ 20, 0, 20 }, hi{ 28, 6, 28 };
        fill({ 18, -1, 18 }, { 30, -1, 30 }, "stone");
        for (const int x : { 21, 25 })
            for (const int z : { 21, 25 }) {
                fill({ x - 1, 3, z - 1 }, { x + 1, 5, z + 1 }, "leaves");
                fill({ x, 0, z }, { x, 3, z }, "log");
            }
        Check(Pump(a, 10.0, [&] { return count(lo, hi, BlockType::LEAVES) > 90; }), "a little wood stands");
        const int leaves = count(lo, hi, BlockType::LEAVES), logs = count(lo, hi, BlockType::LOG);
        fill({ 22, 4, 21 }, { 22, 4, 21 }, "air");
        fill({ 22, 4, 21 }, { 22, 4, 21 }, "fire"); // (in a gap in the first crown)
        Check(Pump(a, 5.0, [&] { return count(lo, hi, BlockType::FIRE) > 3; }), "a fire set in it catches and spreads");
        const bool burnt = Pump(a, 40.0, [&] { return count(lo, hi, BlockType::LEAVES) < leaves / 4; });
        Check(burnt,
              "within a minute the wood is burning down (leaves " + std::to_string(count(lo, hi, BlockType::LEAVES)) + " of " + std::to_string(leaves) + ")");
        Check(count(lo, hi, BlockType::LOG) <= logs, "logs burn too (slower)");
        Check(at({ 21, -1, 21 }) == BlockType::STONE, "the stone floor doesn't burn");
    }

    std::cout << "Bare ground, stone and obsidian" << std::endl;
    {
        fill({ 40, -1, 40 }, { 44, -1, 44 }, "stone");
        fill({ 42, 0, 42 }, { 42, 0, 42 }, "fire");
        fill({ 43, 0, 42 }, { 43, 0, 42 }, "obsidian");
        Check(Pump(a, 5.0, [&] { return at({ 42, 0, 42 }) == BlockType::FIRE; }), "a fire on bare stone");
        Check(Pump(a, 10.0, [&] { return at({ 42, 0, 42 }) == BlockType::AIR; }) && at({ 43, 0, 42 }) == BlockType::OBSIDIAN,
              "with nothing to burn it dies down in a few seconds (and obsidian beside it doesn't burn)");
    }

    std::cout << "Putting it out" << std::endl;
    {
        // A hedge of leaves to keep fires going while they're put out.
        fill({ 50, -1, 50 }, { 58, -1, 54 }, "stone");
        fill({ 50, 0, 52 }, { 58, 0, 52 }, "leaves");
        fill({ 51, 0, 51 }, { 51, 0, 51 }, "fire");
        fill({ 55, 0, 51 }, { 55, 0, 51 }, "fire");
        Check(Pump(a, 5.0, [&] { return at({ 51, 0, 51 }) == BlockType::FIRE && at({ 55, 0, 51 }) == BlockType::FIRE; }), "fires burn by a hedge");
        fill({ 51, 0, 51 }, { 51, 0, 51 }, "water");
        Check(Pump(a, 5.0, [&] { return at({ 51, 0, 51 }) != BlockType::FIRE; }), "water poured on one puts it out");
        fill({ 55, 0, 51 }, { 55, 0, 51 }, "sand");
        Check(Pump(a, 5.0, [&] { return at({ 55, 0, 51 }) == BlockType::SAND; }), "a block put on one smothers it");

        // A bucket: scoop up water from a pool, pour it on a fire.
        server.QueueConsoleCommand("gamemode survival Warden");
        Pump(a, 5.0, [&] { return !a.client.IsCreative(); });
        server.QueueConsoleCommand("tp Warden 54.5 0 49.5");
        Pump(a, 5.0, [&] { return std::abs(a.feet.x - 54.5f) < 0.1f; });
        server.QueueConsoleCommand("give bucket 1 Warden");
        Pump(a, 5.0, [&] { return a.client.GetResources(BlockType::BUCKET) > 0; });
        server.QueueConsoleCommand("tp Warden 46.5 0 47.5");
        Pump(a, 5.0, [&] { return std::abs(a.feet.x - 46.5f) < 0.1f; });
        fill({ 44, -1, 47 }, { 44, -1, 47 }, "water"); // (well away from the water poured above)
        Pump(a, 5.0, [&] { return at({ 44, -1, 47 }) == BlockType::WATER; });
        a.client.UseBucket({ 44, -1, 47 }, false);
        const bool scooped = Pump(a, 5.0, [&] {
            return a.client.GetResources(BlockType::WATER_BUCKET) > 0 && a.client.GetResources(BlockType::BUCKET) == 0 && at({ 44, -1, 47 }) != BlockType::WATER;
        });
        Check(scooped && at({ 44, -1, 47 }) != BlockType::WATER,
              "a bucket scoops up a block of water (buckets " + std::to_string(a.client.GetResources(BlockType::BUCKET)) + ", full " +
                  std::to_string(a.client.GetResources(BlockType::WATER_BUCKET)) + ", the cell now " + std::to_string(static_cast<int>(at({ 44, -1, 47 }))) + ")");
        server.QueueConsoleCommand("tp Warden 54.5 0 49.5");
        Pump(a, 5.0, [&] { return std::abs(a.feet.x - 54.5f) < 0.1f; });
        fill({ 57, 0, 51 }, { 57, 0, 51 }, "fire");
        fill({ 56, 0, 51 }, { 56, 0, 51 }, "fire");
        Pump(a, 5.0, [&] { return at({ 57, 0, 51 }) == BlockType::FIRE && at({ 56, 0, 51 }) == BlockType::FIRE; });
        a.client.UseBucket({ 57, 0, 51 }, true);
        Check(Pump(a, 5.0, [&] { return at({ 57, 0, 51 }) != BlockType::FIRE && at({ 56, 0, 51 }) != BlockType::FIRE; }) &&
                  a.client.GetResources(BlockType::BUCKET) > 0,
              "poured on a fire, it puts out that one and those round it (and the bucket's empty again)");
    }

    std::cout << "Burning" << std::endl;
    {
        // A survival player walks into fire: burned, and still burning once out of it.
        fill({ 60, -1, 60 }, { 66, -1, 64 }, "stone");
        fill({ 61, 0, 61 }, { 61, 1, 61 }, "fire");
        server.QueueConsoleCommand("tp Warden 61.5 0 61.5");
        Pump(a, 5.0, [&] { return std::abs(a.feet.x - 61.5f) < 0.1f; });
        const int before = a.client.GetHealth();
        Check(Pump(a, 5.0, [&] { return a.client.GetHealth() < before; }), "standing in fire burns");
        Check(a.client.Burning(), "and the client is told it's on fire (flames round the screen)");
        server.QueueConsoleCommand("tp Warden 64.5 0 63.5"); // (out of it)
        Pump(a, 5.0, [&] { return std::abs(a.feet.x - 64.5f) < 0.1f; });
        Pump(a, 0.6, [] { return false; });
        const int out = a.client.GetHealth();
        Check(Pump(a, 3.0, [&] { return a.client.GetHealth() < out; }), "and you go on burning after stepping out");
        fill({ 64, 0, 63 }, { 64, 1, 63 }, "water");
        Pump(a, 1.0, [] { return false; });
        const int wet = a.client.GetHealth();
        Pump(a, 2.0, [] { return false; });
        Check(a.client.GetHealth() >= wet, "until you're in water");
        Check(!a.client.Burning(), "which puts it out on the client too");

        // Items in it are lost.
        server.QueueConsoleCommand("gamemode creative Warden");
        Pump(a, 5.0, [&] { return a.client.IsCreative(); });
        server.QueueConsoleCommand("give dirt 1 Warden");
        Pump(a, 5.0, [&] { return a.client.GetResources(BlockType::DIRT) > 0; });
        fill({ 62, 0, 62 }, { 62, 0, 62 }, "fire");
        fill({ 62, -1, 62 }, { 62, -1, 62 }, "leaves"); // (to keep it burning)
        Pump(a, 5.0, [&] { return at({ 62, 0, 62 }) == BlockType::FIRE; });
        int slot = -1;
        const auto& slots = a.client.GetInventory().slots;
        for (size_t i = 0; i < slots.size(); i++)
            if (!slots[i].Empty() && slots[i].item == Items::OfBlock(BlockType::DIRT)) slot = static_cast<int>(i);
        server.QueueConsoleCommand("tp Warden 62.5 0 62.5"); // (in it: creative players aren't burned)
        Pump(a, 5.0, [&] { return std::abs(a.feet.z - 62.5f) < 0.1f; });
        a.client.DropItem(net::Box::Player, slot, 1, glm::vec3(0.0f, -1.0f, 0.0f));
        bool seen = false;
        Pump(a, 3.0, [&] {
            for (const auto& i : a.client.GetItems()) seen |= i.item == Items::OfBlock(BlockType::DIRT);
            return seen;
        });
        Check(Pump(a, 6.0, [&] {
                  for (const auto& i : a.client.GetItems())
                      if (i.item == Items::OfBlock(BlockType::DIRT)) return false;
                  return true;
              }),
              "an item thrown into fire is lost");
    }

    std::cout << "Sprinklers and heat sensors" << std::endl;
    {
        auto signalOn = [&](const glm::ivec3& p) {
            auto it = a.client.GetSignals().find(p);
            return it != a.client.GetSignals().end() && it->second.on != 0;
        };
        auto configure = [&](const glm::ivec3& p, Signals::Kind kind, uint8_t listen, uint8_t b, uint8_t out) {
            net::SignalInfo info;
            info.pos = p;
            info.kind = static_cast<uint8_t>(kind);
            info.a = listen;
            info.b = b;
            info.out = out;
            a.client.ConfigureSignal(info);
            return Pump(a, 5.0, [&] {
                auto it = a.client.GetSignals().find(p);
                return it != a.client.GetSignals().end() && it->second.a == listen && it->second.b == b && it->second.out == out;
            });
        };
        // On its own: a sprinkler under a tank of water, over a burning log and leaves.
        fill({ 78, -1, 78 }, { 108, -1, 108 }, "stone");
        fill({ 84, 5, 84 }, { 84, 5, 84 }, "fluid_tank");
        fill({ 84, 4, 84 }, { 84, 4, 84 }, "sprinkler");
        fill({ 82, 0, 84 }, { 82, 0, 84 }, "log");
        fill({ 86, 0, 84 }, { 86, 0, 84 }, "leaves");
        Pump(a, 5.0, [&] { return at({ 84, 4, 84 }) == BlockType::SPRINKLER && at({ 86, 0, 84 }) == BlockType::LEAVES; });
        server.QueueConsoleCommand("fluid 84 5 84 water");
        server.QueueConsoleCommand("tp Warden 84.5 0 88.5");
        Pump(a, 5.0, [&] { return std::abs(a.feet.z - 88.5f) < 0.1f; });
        fill({ 82, 1, 84 }, { 82, 1, 84 }, "fire");
        fill({ 86, 1, 84 }, { 86, 1, 84 }, "fire");
        Check(Pump(a, 3.0, [&] { return count({ 80, 0, 82 }, { 88, 2, 86 }, BlockType::FIRE) == 0; }) && at({ 82, 0, 84 }) == BlockType::LOG,
              "a sprinkler with water puts out the fires below it by itself, before the log burns");
        fill({ 86, 1, 84 }, { 86, 1, 84 }, "fire");
        Check(Pump(a, 3.0, [&] { return at({ 86, 1, 84 }) != BlockType::FIRE; }), "and again when fire comes back");

        // On a channel: it waits for it. A heat sensor on that channel, seeing fire,
        // sets it off.
        const glm::ivec3 sprinkler{ 100, 4, 100 }, sensor{ 100, 0, 95 };
        fill(sprinkler + glm::ivec3(0, 1, 0), sprinkler + glm::ivec3(0, 1, 0), "fluid_tank");
        fill(sprinkler, sprinkler, "sprinkler");
        fill(sensor, sensor, "heat_sensor");
        fill({ 102, 0, 100 }, { 102, 0, 100 }, "log");
        fill({ 100, 0, 96 }, { 100, 0, 96 }, "log");
        server.QueueConsoleCommand("tp Warden 98.5 0 98.5");
        Pump(a, 5.0, [&] { return std::abs(a.feet.z - 98.5f) < 0.1f && at(sensor) == BlockType::HEAT_SENSOR && at(sprinkler) == BlockType::SPRINKLER; });
        server.QueueConsoleCommand("fluid 100 5 100 water");
        Check(configure(sprinkler, Signals::Kind::Sprinkler, 5, 0, 0) && configure(sensor, Signals::Kind::HeatSensor, 0, 2, 0),
              "a sprinkler listens to 5; a heat sensor (range 2) sends on nothing yet");
        Check(!signalOn(sensor), "with no fire near, the heat sensor is off");
        fill({ 102, 1, 100 }, { 102, 1, 100 }, "fire");
        fill({ 100, 1, 96 }, { 100, 1, 96 }, "fire");
        Check(Pump(a, 3.0, [&] { return signalOn(sensor); }), "fire near the heat sensor turns it on");
        Pump(a, 1.0, [] { return false; });
        Check(count({ 96, 0, 96 }, { 104, 3, 104 }, BlockType::FIRE) > 0, "on a channel that's off, the sprinkler leaves fire in its reach alone");
        Check(configure(sensor, Signals::Kind::HeatSensor, 0, 2, 5), "the heat sensor now sends on 5");
        Check(Pump(a, 3.0, [&] { return count({ 96, 0, 96 }, { 104, 3, 104 }, BlockType::FIRE) == 0; }), "which sets the sprinkler off, putting the fires out");
        Check(Pump(a, 3.0, [&] { return !signalOn(sensor); }), "and with the fire out, the sensor goes off");
    }

    a.client.Disconnect();
    Pump(a, 0.3, [] { return false; });
    server.Stop();
    std::cout << (failures ? "FIRE TEST FAILED (" : "ALL FIRE CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
