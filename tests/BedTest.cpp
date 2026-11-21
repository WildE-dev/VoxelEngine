// Beds over real connections. Checks that
//  - a bed is crafted (3 planks and a hide) and placed as a half-height block,
//  - right-clicking it (UseBed) makes it where you respawn, and by day says you can
//    only sleep at night,
//  - at night one player alone sleeps through to the morning; with two, it's morning
//    only once both are in bed,
//  - a player who dies comes back at their bed, still after a restart,
//  - with the bed gone, they're back at the world's spawn point, and told why,
//  - doors: placed two tall, opened and closed (not on anyone), broken as a whole.
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

// Test clients ask for a short view (chunks): plenty for the checks, and light enough
// to run every suite at once.
constexpr int kTestViewDistance = 4;

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

// A client that stays wherever the server puts it.
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

size_t Count(const TestClient& c, const std::string& text) {
    size_t n = 0;
    for (const auto& line : c.client.GetChat()) n += line.text.find(text) != std::string::npos;
    return n;
}

Server::Options MakeOptions(const fs::path& dir) {
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = std::getenv("BED_LOG") != nullptr;
    options.worldDirectory = dir.string();
    options.worldType = TerrainGenerator::Type::Empty;
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Survival;
    options.dayLengthSeconds = 100000.0f; // the clock stays where /time puts it
    return options;
}

// Whether the client's clock (0..1, 0 sunrise, 0.5 sunset) says it's night.
bool Night(const Client& c) { return c.GetTimeOfDay() > 0.5f; }

} // namespace

int main() {
    constexpr uint32_t kBlock = Block::kUnitsPerBlock;
    const fs::path dir = fs::temp_directory_path() / ("voxel_bed_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const glm::ivec3 bedAt{ 20, 0, 20 }; // away from the world's spawn point (7.5, 0, 7.5)
    {
        Server server;
        if (!server.Start(MakeOptions(dir))) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a, b;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Sleeper", error, kTestViewDistance);
        auto one = [&](double s, const std::function<bool()>& done) { return Pump({ &a }, s, done); };
        Check(one(20.0, [&] { return a.joined; }), "a survival player joins");
        server.QueueConsoleCommand("tp Sleeper 18.5 0 20.5");
        server.QueueConsoleCommand("give planks 3 Sleeper");
        server.QueueConsoleCommand("give hide 1 Sleeper");
        one(20.0, [&] { return a.feet.y < 0.5f && a.client.GetResources(BlockType::HIDE) == kBlock; });

        std::cout << "Making and placing" << std::endl;
        a.client.Craft(Crafting::Find("bed"), 1);
        Check(one(20.0, [&] { return a.client.GetResources(BlockType::BED) == kBlock; }), "3 planks and a hide make a bed");
        a.client.PlaceBlock(bedAt, BlockType::BED);
        Block bed;
        Check(one(20.0, [&] { return a.world.GetBlock(bedAt.x, bedAt.y, bedAt.z, bed) && bed.type == BlockType::BED; }) && bed.ResourceUnits() == kBlock &&
                  bed.edgeData.GetTopY(0) == 4,
              "it's placed, half a block high (and worth a whole bed)");

        std::cout << "Sleeping" << std::endl;
        server.QueueConsoleCommand("time noon");
        one(20.0, [&] { return !Night(a.client); });
        a.client.UseBed(bedAt);
        Check(one(20.0, [&] { return Count(a, "respawn at this bed") == 1 && Count(a, "only sleep at night") == 1; }),
              "by day, using it sets where they respawn, but they can't sleep");
        server.QueueConsoleCommand("time midnight");
        one(20.0, [&] { return Night(a.client); });
        a.client.UseBed(bedAt);
        Check(one(20.0, [&] { return Count(a, "You slept: it's morning") == 1 && !Night(a.client); }), "at night, alone, they sleep till morning");

        b.client.Connect("127.0.0.1", server.GetPort(), "Owl", error, kTestViewDistance);
        auto both = [&](double s, const std::function<bool()>& done) { return Pump({ &a, &b }, s, done); };
        Check(both(20.0, [&] { return b.joined; }), "a second player joins");
        server.QueueConsoleCommand("tp Owl 21.5 0 21.5");
        server.QueueConsoleCommand("time midnight");
        both(20.0, [&] { return b.feet.y < 0.5f && Night(a.client) && Night(b.client); });
        a.client.UseBed(bedAt);
        Check(both(20.0, [&] { return Count(b, "Sleeper is in bed (1 of 2 asleep)") == 1; }), "with two, one in bed isn't enough");
        Check(Night(a.client), "it's still night");
        b.client.UseBed(bedAt);
        Check(both(20.0, [&] { return Count(a, "Everyone slept: it's morning") == 1 && !Night(a.client); }), "once both are in bed, it's morning");

        std::cout << "Doors" << std::endl;
        const glm::ivec3 doorAt{ 17, 0, 22 };
        auto typeAt = [&](const glm::ivec3& p) { Block t; return a.world.GetBlock(p.x, p.y, p.z, t) ? t.type : BlockType::COUNT; };
        const glm::ivec3 upper = doorAt + glm::ivec3(0, 1, 0);
        server.QueueConsoleCommand("give door 1 Sleeper");
        both(20.0, [&] { return a.client.GetResources(BlockType::DOOR) == kBlock; });
        a.client.PlaceBlock(doorAt, BlockType::DOOR);
        Check(both(20.0, [&] { return typeAt(doorAt) == BlockType::DOOR && typeAt(upper) == BlockType::DOOR; }) && a.client.GetResources(BlockType::DOOR) == 0,
              "a door is placed two blocks tall, for one door");
        a.client.UseBed(doorAt);
        Check(both(20.0, [&] { return typeAt(doorAt) == BlockType::DOOR_OPEN && typeAt(upper) == BlockType::DOOR_OPEN; }), "right-clicking opens both halves");
        server.QueueConsoleCommand("tp Owl 17.5 0 22.5"); // in the doorway
        both(20.0, [&] { return std::abs(b.feet.x - 17.5f) < 0.1f && std::abs(b.feet.z - 22.5f) < 0.1f; });
        a.client.UseBed(upper);
        both(1.0, [] { return false; });
        Check(typeAt(doorAt) == BlockType::DOOR_OPEN, "it won't close on someone standing in it");
        server.QueueConsoleCommand("tp Owl 21.5 0 21.5");
        both(20.0, [&] { return std::abs(b.feet.x - 21.5f) < 0.1f; });
        a.client.UseBed(upper);
        Check(both(20.0, [&] { return typeAt(doorAt) == BlockType::DOOR && typeAt(upper) == BlockType::DOOR; }), "once they're out, it closes (from either half)");
        server.QueueConsoleCommand("gamemode creative Sleeper");
        both(20.0, [&] { return a.client.IsCreative(); });
        a.client.BreakBlock(upper);
        Check(both(20.0, [&] { return typeAt(doorAt) == BlockType::AIR && typeAt(upper) == BlockType::AIR; }), "breaking either half removes the whole door");
        server.QueueConsoleCommand("gamemode survival Sleeper");
        both(20.0, [&] { return !a.client.IsCreative(); });

        std::cout << "Respawning" << std::endl;
        auto kill = [&](TestClient& victim, const std::string& name, TestClient& killer, const std::string& killerName) {
            const size_t before = Count(victim, name + " was slain");
            for (int i = 0; i < 30 && Count(victim, name + " was slain") == before; i++) {
                server.QueueConsoleCommand("tp " + killerName + " " + std::to_string(victim.feet.x - 1.5f) + " " + std::to_string(victim.feet.y) + " " +
                                           std::to_string(victim.feet.z));
                Pump({ &victim, &killer }, 0.4, [] { return false; });
                killer.client.AttackPlayer(victim.client.GetPlayerId(), Tool::None);
                Pump({ &victim, &killer }, 0.4, [] { return false; });
            }
            return Count(victim, name + " was slain") > before;
        };
        server.QueueConsoleCommand("tp Sleeper 40.5 0 40.5");
        both(20.0, [&] { return a.feet.x > 40.0f; });
        Check(kill(a, "Sleeper", b, "Owl"), "the sleeper is slain far from the bed");
        auto nearBed = [&](const TestClient& c) { return glm::distance(c.feet, glm::vec3(bedAt) + glm::vec3(0.5f, 0.55f, 0.5f)) < 0.1f; };
        Check(both(20.0, [&] { return nearBed(a); }), "and comes back on their bed");
        server.QueueConsoleCommand("save");
        both(2.0, [] { return false; });
        a.client.Disconnect();
        b.client.Disconnect();
        Pump({ &a, &b }, 0.3, [] { return false; });
        server.Stop();
    }
    {
        Server server;
        if (!server.Start(MakeOptions(dir))) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a, b;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Sleeper", error, kTestViewDistance);
        b.client.Connect("127.0.0.1", server.GetPort(), "Owl", error, kTestViewDistance);
        auto both = [&](double s, const std::function<bool()>& done) { return Pump({ &a, &b }, s, done); };
        Check(both(20.0, [&] { return a.joined && b.joined; }), "after a restart, both come back");
        auto kill = [&]() {
            const size_t before = Count(a, "Sleeper was slain");
            for (int i = 0; i < 30 && Count(a, "Sleeper was slain") == before; i++) {
                server.QueueConsoleCommand("tp Owl " + std::to_string(a.feet.x - 1.5f) + " " + std::to_string(a.feet.y) + " " + std::to_string(a.feet.z));
                both(0.4, [] { return false; });
                b.client.AttackPlayer(a.client.GetPlayerId(), Tool::None);
                both(0.4, [] { return false; });
            }
            return Count(a, "Sleeper was slain") > before;
        };
        server.QueueConsoleCommand("tp Sleeper 40.5 0 40.5");
        both(20.0, [&] { return a.feet.x > 40.0f; });
        Check(kill(), "slain again");
        Check(both(20.0, [&] { return glm::distance(a.feet, glm::vec3(bedAt) + glm::vec3(0.5f, 0.55f, 0.5f)) < 0.1f; }), "they still come back on their bed");
        server.QueueConsoleCommand("gamemode creative Owl");
        server.QueueConsoleCommand("tp Owl 22.5 0 20.5");
        both(20.0, [&] { return b.client.IsCreative() && b.feet.x > 22.0f; });
        b.client.BreakBlock(bedAt);
        Check(both(20.0, [&] { Block g; return a.world.GetBlock(bedAt.x, bedAt.y, bedAt.z, g) && g.IsAir(); }), "the bed is broken");
        server.QueueConsoleCommand("gamemode survival Owl");
        server.QueueConsoleCommand("tp Sleeper 40.5 0 40.5");
        both(20.0, [&] { return !b.client.IsCreative() && a.feet.x > 40.0f; });
        Check(kill(), "slain once more");
        Check(both(20.0, [&] { return glm::distance(glm::vec2(a.feet.x, a.feet.z), glm::vec2(7.5f)) < 0.5f && Count(a, "Your bed is gone") == 1; }),
              "with no bed, back at the spawn point, and told why");
        a.client.Disconnect();
        b.client.Disconnect();
        Pump({ &a, &b }, 0.3, [] { return false; });
        server.Stop();
    }
    fs::remove_all(dir);
    std::cout << (failures ? "BED TEST FAILED (" : "ALL BED CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
