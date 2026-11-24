// Food and hunger over a real connection. Checks that
//  - a survival player gets hungry over time, and starving hurts them down to the
//    last heart but no further,
//  - eating food they hold fills them up (cooked venison more than raw) and uses it
//    up; they can't eat what they don't have, or when full,
//  - health only comes back while well fed,
//  - killing a deer gives raw venison, which cooks (with a little wood) into cooked
//    venison,
//  - items can't be placed as blocks,
//  - creative players don't get hungry,
//  - farming: seeds grow only on soil, ripen into wheat, harvest into wheat and seeds,
//    and 3 wheat bake into bread.
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "TestSections.h"
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
    BlockType held = BlockType::AIR; // what others (and the server) see in hand
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

int main(int argc, char** argv) {
    TestSections sections(argc, argv);
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
    options.cropGrowthRate = 200.0f; // crops ripen within a second or two
    options.fishingRate = 20.0f;     // fish bite within a second
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

    if (sections.Run("hunger")) {
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

    }
    if (sections.Run("food")) {
        std::cout << "Hunting and cooking" << std::endl;
        server.QueueConsoleCommand("summon deer " + std::to_string(a.feet.x + 2.0f) + " " + std::to_string(a.feet.y) + " " + std::to_string(a.feet.z));
        Check(Pump(a, 20.0, [&] { return FindDeer(a) != nullptr; }), "a deer is summoned beside them");
        const uint32_t rawBeforeHunt = a.client.GetResources(BlockType::RAW_VENISON);
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
        Check(gone && Pump(a, 5.0, [&] { return a.client.GetResources(BlockType::RAW_VENISON) >= rawBeforeHunt + Block::kUnitsPerBlock; }),
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

        std::cout << "Farming" << std::endl;
        server.QueueConsoleCommand("tp Eater 7.5 0 7.5");
        server.QueueConsoleCommand("give seeds 2 Eater");
        server.QueueConsoleCommand("food 10 Eater");
        sync();
        Pump(a, 20.0, [&] { return a.feet.y < 0.5f; });
        a.client.PlaceBlock({ 9, 3, 7 }, BlockType::CROP);
        wait(1.0);
        Block planted;
        Check(a.world.GetBlock(9, 3, 7, planted) && planted.IsAir() && a.client.GetResources(BlockType::SEEDS) == 2 * Block::kUnitsPerBlock,
              "seeds won't grow in mid air");
        a.client.PlaceBlock({ 9, 0, 7 }, BlockType::CROP);
        Check(Pump(a, 20.0, [&] { return a.world.GetBlock(9, 0, 7, planted) && planted.type == BlockType::CROP; }) &&
                  a.client.GetResources(BlockType::SEEDS) == Block::kUnitsPerBlock,
              "a seed planted on grass comes up as a crop");
        Check(Pump(a, 20.0, [&] { return a.world.GetBlock(9, 0, 7, planted) && planted.type == BlockType::RIPE_WHEAT; }), "which ripens into wheat");
        a.client.BreakBlock({ 9, 0, 7 });
        // (The client counts the wheat at once; the seeds come with the server's answer.)
        Check(Pump(a, 20.0, [&] { return a.client.GetResources(BlockType::WHEAT) == Block::kUnitsPerBlock && a.client.GetResources(BlockType::SEEDS) >= 2 * Block::kUnitsPerBlock; }),
              "harvested, it gives wheat and seeds to plant again (wheat " + std::to_string(a.client.GetResources(BlockType::WHEAT)) + ", seeds " +
                  std::to_string(a.client.GetResources(BlockType::SEEDS)) + ")");
        server.QueueConsoleCommand("give sapling 1 Eater");
        sync();
        a.client.PlaceBlock({ 5, 0, 7 }, BlockType::SAPLING);
        Check(Pump(a, 20.0, [&] { Block t; return a.world.GetBlock(5, 0, 7, t) && t.type == BlockType::SAPLING; }), "a sapling planted on grass");
        Check(Pump(a, 30.0, [&] {
                  Block trunk, top;
                  return a.world.GetBlock(5, 0, 7, trunk) && trunk.type == BlockType::LOG && a.world.GetBlock(5, 5, 7, top) && (top.type == BlockType::LEAVES || top.type == BlockType::LOG);
              }),
              "grows into a tree");

        server.QueueConsoleCommand("give wheat 2 Eater");
        sync();
        a.client.Craft(Crafting::Find("bread"), 1);
        Check(Pump(a, 20.0, [&] { return a.client.GetResources(BlockType::BREAD) == Block::kUnitsPerBlock && a.client.GetResources(BlockType::WHEAT) == 0; }),
              "3 wheat bake into bread");
        const int beforeBread = a.client.GetFood();
        a.client.Eat(BlockType::BREAD);
        Check(Pump(a, 20.0, [&] { return a.client.GetFood() >= beforeBread + 5; }), "which is good for 6 food");

        std::cout << "Fishing" << std::endl;
        // A one-block pond (dug and filled in creative), then back to survival with a rod.
        server.QueueConsoleCommand("gamemode creative Eater");
        sync();
        a.client.BreakBlock({ 9, -1, 9 });
        Pump(a, 20.0, [&] { Block g; return a.world.GetBlock(9, -1, 9, g) && g.IsAir(); });
        a.client.PlaceBlock({ 9, -1, 9 }, BlockType::WATER);
        Pump(a, 20.0, [&] { Block g; return a.world.GetBlock(9, -1, 9, g) && g.HasWater(); });
        server.QueueConsoleCommand("gamemode survival Eater");
        server.QueueConsoleCommand("give rod Eater");
        sync();
        Pump(a, 20.0, [&] { return a.client.HasTool(Tool::Rod) && !a.client.IsCreative(); });
        a.client.Cast({ 9, 0, 9 }, true); // (dry ground: no)
        wait(0.5);
        Check(!a.client.GetBobber(), "a line cast onto dry ground doesn't stay out");
        a.client.Cast({ 9, -1, 9 }, true);
        Check(Pump(a, 20.0, [&] { return a.client.GetBobber().has_value(); }), "cast into the pond, the bobber floats");
        Check(Pump(a, 20.0, [&] { return a.client.GetResources(BlockType::RAW_FISH) == Block::kUnitsPerBlock; }) && a.client.TakeBite(),
              "a fish bites, and it's caught");
        Check(!a.client.GetBobber() && Has(a, "Angler: catch a fish"), "the line comes in (and that's an achievement)");
        server.QueueConsoleCommand("give log 1 Eater");
        sync();
        a.client.Craft(Crafting::Find("cooked_fish"), 1);
        Check(Pump(a, 20.0, [&] { return a.client.GetResources(BlockType::COOKED_FISH) == Block::kUnitsPerBlock; }), "and cooks with a little wood");

        std::cout << "Campfire" << std::endl;
        server.QueueConsoleCommand("give campfire 1 Eater");
        server.QueueConsoleCommand("give raw_venison 2 Eater");
        sync();
        a.client.PlaceBlock({ 6, 0, 7 }, BlockType::CAMPFIRE);
        int blockLight = 0, skyLight = 0;
        Check(Pump(a, 20.0, [&] { return a.world.GetLight(6, 0, 7, blockLight, skyLight) && blockLight >= 13; }), "a campfire burns bright (light " + std::to_string(blockLight) + ")");
        a.held = BlockType::RAW_VENISON;
        wait(0.3);
        const uint32_t cookedBefore = a.client.GetResources(BlockType::COOKED_VENISON), rawBefore = a.client.GetResources(BlockType::RAW_VENISON);
        a.client.UseBed({ 6, 0, 7 });
        Check(Pump(a, 20.0, [&] { return a.client.GetResources(BlockType::COOKED_VENISON) == cookedBefore + Block::kUnitsPerBlock; }) &&
                  a.client.GetResources(BlockType::RAW_VENISON) == rawBefore - Block::kUnitsPerBlock,
              "right-clicking it with raw venison in hand cooks one");
        a.held = BlockType::AIR;

        std::cout << "Achievements" << std::endl;
        // (Eating is in the hunger group: its First Meal only when that ran too.)
        const bool ate = sections.Run("hunger");
        Check((!ate || Has(a, "First Meal: eat something")) && Has(a, "Hunter: bring down a deer") && Has(a, "Cook: cook some venison") &&
                  Has(a, "Farmer: harvest ripe wheat") && Has(a, "Baker: bake bread"),
              "eating, hunting, cooking, farming and baking each earn an achievement");
        a.client.SendChat("/achievements");
        Check(Pump(a, 20.0, [&] { return Has(a, " of 22 achievements") && Has(a, "Still to do:"); }), "/achievements lists the earned ones and what's still to do");
        if (ate) {
            size_t meals = 0;
            for (const auto& line : a.client.GetChat()) meals += line.text.find("First Meal: eat") != std::string::npos;
            Check(meals == 1, "each is earned only once (" + std::to_string(meals) + " first meals, after three)");
        }

        std::cout << "Creative" << std::endl;
        server.QueueConsoleCommand("gamemode creative Eater");
        server.QueueConsoleCommand("food 10 Eater");
        sync();
        wait(7.0);
        Check(a.client.GetFood() == 10, "creative players don't get hungry");

    }
    a.client.Disconnect();
    Pump(a, 0.3, [] { return false; });
    server.Stop();
    if (!sections.Valid()) failures++;
    std::cout << (failures ? "FOOD TEST FAILED (" : "ALL FOOD CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
