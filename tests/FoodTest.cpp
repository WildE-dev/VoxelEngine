// Food and hunger over a real connection. Checks that
//  - a survival player gets hungry over time, and starving hurts them down to the
//    last heart but no further,
//  - eating food they hold fills them up (cooked venison more than raw) and uses it
//    up; they can't eat what they don't have, or when full,
//  - health only comes back while well fed,
//  - killing a deer gives raw venison, which cooks (with a little wood) into cooked
//    venison,
//  - items can't be placed as blocks,
//  - creative players don't get hungry.
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "world/Crafting.h"
#include "world/Hunger.h"

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

bool Pump(TestClient& c, double seconds, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        c.Update();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

bool Has(const TestClient& c, const std::string& text) {
    for (const auto& line : c.client.GetChat())
        if (line.text.find(text) != std::string::npos) return true;
    return false;
}

const Client::Creature* FindDeer(const TestClient& c) {
    static std::vector<Client::Creature> seen;
    seen = c.client.GetCreatures();
    for (const auto& d : seen)
        if (d.type == CreatureType::Deer) return &d;
    return nullptr;
}

} // namespace

int main() {
    Server server;
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = false;
    options.worldType = TerrainGenerator::Type::Empty;
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Survival;
    options.dayLengthSeconds = 100000.0f;
    options.hungerRate = 10.0f; // a point every 5 s; starving hurts every 0.4 s
    if (!server.Start(options)) return EXIT_FAILURE;
    server.RunInBackground();

    TestClient a;
    std::string error;
    a.client.Connect("127.0.0.1", server.GetPort(), "Eater", error, kTestViewDistance);
    auto wait = [&](double s) { Pump(a, s, [] { return false; }); };
    // Commands run in order: a reply to a later one means the earlier ones are done.
    int syncs = 0;
    auto sync = [&] {
        server.QueueConsoleCommand("msg Eater sync" + std::to_string(++syncs));
        return Pump(a, 20.0, [&] { return Has(a, "sync" + std::to_string(syncs)); });
    };
    Check(Pump(a, 20.0, [&] { return a.joined && a.client.GetFood() == Hunger::kMaxFood; }), "a survival player joins, full");
    server.QueueConsoleCommand("time midnight"); // (no burning ghouls, nothing else going on)

    std::cout << "Hunger" << std::endl;
    Check(Pump(a, 20.0, [&] { return a.client.GetFood() < Hunger::kMaxFood; }), "they get hungry over time");
    server.QueueConsoleCommand("food 0 Eater");
    Check(Pump(a, 20.0, [&] { return a.client.GetFood() == 0 && a.client.GetHealth() < 20; }), "with no food, starving hurts");
    Check(Pump(a, 30.0, [&] { return a.client.GetHealth() == Hunger::kStarveFloor; }), "down to the last heart");
    wait(2.0);
    Check(a.client.GetHealth() == Hunger::kStarveFloor, "but no further");

    std::cout << "Eating" << std::endl;
    a.client.Eat(BlockType::COOKED_VENISON);
    wait(1.0);
    Check(a.client.GetFood() == 0, "they can't eat food they don't have");
    server.QueueConsoleCommand("give cooked_venison 2 Eater");
    server.QueueConsoleCommand("give raw_venison 1 Eater");
    sync();
    Check(a.client.GetResources(BlockType::COOKED_VENISON) == 2 * Block::kUnitsPerBlock, "/give hands out food");
    a.client.Eat(BlockType::RAW_VENISON);
    Check(Pump(a, 20.0, [&] { return a.client.GetFood() == 3; }), "raw venison gives 3 food");
    a.client.Eat(BlockType::COOKED_VENISON);
    a.client.Eat(BlockType::COOKED_VENISON);
    Check(Pump(a, 20.0, [&] { return a.client.GetFood() >= 17; }), "cooked venison gives 8 each");
    Check(a.client.GetResources(BlockType::COOKED_VENISON) == 0 && a.client.GetResources(BlockType::RAW_VENISON) == 0, "and eating uses it up");
    const int fed = a.client.GetHealth();
    Check(Pump(a, 20.0, [&] { return a.client.GetHealth() > fed; }), "well fed, health comes back");
    server.QueueConsoleCommand("food 20 Eater");
    server.QueueConsoleCommand("give raw_venison 1 Eater");
    sync();
    a.client.Eat(BlockType::RAW_VENISON);
    wait(1.0);
    Check(a.client.GetResources(BlockType::RAW_VENISON) == Block::kUnitsPerBlock, "a full player can't eat");

    std::cout << "Items aren't blocks" << std::endl;
    a.client.PlaceBlock({ 3, 0, 3 }, BlockType::RAW_VENISON);
    wait(1.0);
    Block placed;
    Check(a.world.GetBlock(3, 0, 3, placed) && placed.IsAir() && a.client.GetResources(BlockType::RAW_VENISON) == Block::kUnitsPerBlock,
          "raw venison can't be placed (and isn't used up trying)");

    std::cout << "Hunting and cooking" << std::endl;
    server.QueueConsoleCommand("summon deer " + std::to_string(a.feet.x + 2.0f) + " " + std::to_string(a.feet.y) + " " + std::to_string(a.feet.z));
    Check(Pump(a, 20.0, [&] { return FindDeer(a) != nullptr; }), "a deer is summoned beside them");
    bool gone = false;
    for (int i = 0; i < 12 && !gone; i++) {
        const auto* d = FindDeer(a);
        if (!d) break;
        const uint16_t id = d->id;
        server.QueueConsoleCommand("tp Eater " + std::to_string(d->feet.x - 1.5f) + " " + std::to_string(d->feet.y) + " " + std::to_string(d->feet.z));
        wait(0.5);
        a.client.Attack(id, Tool::None);
        gone = Pump(a, 1.0, [&] { return FindDeer(a) == nullptr; });
    }
    Check(gone && Pump(a, 5.0, [&] { return a.client.GetResources(BlockType::RAW_VENISON) >= 2 * Block::kUnitsPerBlock; }),
          "killing it gives raw venison");
    server.QueueConsoleCommand("give log 1 Eater");
    sync();
    const int cook = Crafting::Find("cooked_venison");
    const uint32_t raw = a.client.GetResources(BlockType::RAW_VENISON);
    a.client.Craft(cook, 1);
    Check(Pump(a, 20.0, [&] { return a.client.GetResources(BlockType::COOKED_VENISON) == Block::kUnitsPerBlock; }) &&
              a.client.GetResources(BlockType::RAW_VENISON) == raw - Block::kUnitsPerBlock &&
              a.client.GetResources(BlockType::LOG) == Block::kUnitsPerBlock * 3 / 4,
          "raw venison and a quarter log cook into cooked venison");

    std::cout << "Creative" << std::endl;
    server.QueueConsoleCommand("gamemode creative Eater");
    server.QueueConsoleCommand("food 10 Eater");
    sync();
    wait(7.0);
    Check(a.client.GetFood() == 10, "creative players don't get hungry");

    a.client.Disconnect();
    Pump(a, 0.3, [] { return false; });
    server.Stop();
    std::cout << (failures ? "FOOD TEST FAILED (" : "ALL FOOD CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
