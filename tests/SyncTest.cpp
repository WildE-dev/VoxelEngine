// Two players over real connections, checking the newer systems look the same to both:
//  - a multiblock structure one forms is formed for the other too, and taken apart
//    when the other breaks a part of it;
//  - what one puts in a chest the other finds there, and takes out;
//  - a fluid tank one fills shows what it holds to the other;
//  - the dwarf miners a mining post hires turn up for both, and both see them dig
//    (their "work" animation);
//  - a Dwarf King one hits shows the hurt (his health bar) to the other;
//  - a guard one hires (from a trader) wears the band for both and follows the one who
//    hired them, not the other.
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
#include "world/Creatures.h"
#include "world/Fluids.h"
#include "world/Trades.h"

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
    ItemRef held;
    void Update() {
        client.Poll();
        if (!joined && client.IsWelcomed()) joined = true, feet = client.GetSpawn();
        if (auto c = client.TakeCorrection()) feet = *c;
        if (!joined) return;
        world.Update(feet + glm::vec3(0.0f, 1.62f, 0.0f));
        client.SendPlayerState(feet, glm::vec3(0.0f), 0.0f, 0.0f, net::kFlagOnGround, Tool::None, held);
    }
    BlockType BlockAt(const glm::ivec3& p) {
        Block b;
        return world.GetBlock(p.x, p.y, p.z, b) ? b.type : BlockType::COUNT;
    }
    std::optional<Client::Creature> Find(CreatureType type) const {
        for (const auto& c : client.GetCreatures())
            if (c.type == type) return c;
        return std::nullopt;
    }
};

bool Has(const Client& c, const std::string& text) {
    for (const auto& line : c.GetChat())
        if (line.text.find(text) != std::string::npos) return true;
    return false;
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / ("voxel_sync_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        Server::Options options;
        options.bindHost = "127.0.0.1";
        options.port = 0;
        options.viewDistance = kTestViewDistance;
        options.log = std::getenv("SYNC_LOG") != nullptr;
        options.worldDirectory = dir.string();
        options.worldType = TerrainGenerator::Type::Empty;
        options.weather = Server::Options::Weather::Clear;
        options.defaultMode = net::GameMode::Creative;
        options.creatures = false;
        Server server;
        if (!server.Start(options)) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a, b;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Builder", error, kTestViewDistance);
        b.client.Connect("127.0.0.1", server.GetPort(), "Watcher", error, kTestViewDistance);
        auto both = [&](double seconds, const std::function<bool()>& done) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (std::chrono::steady_clock::now() < deadline) {
                a.Update();
                b.Update();
                if (done()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return done();
        };
        auto wait = [&](double s) { both(s, [] { return false; }); };
        auto fill = [&](const glm::ivec3& lo, const glm::ivec3& hi, const char* block) {
            server.QueueConsoleCommand("fill " + std::to_string(lo.x) + " " + std::to_string(lo.y) + " " + std::to_string(lo.z) + " " + std::to_string(hi.x) + " " +
                                       std::to_string(hi.y) + " " + std::to_string(hi.z) + " " + block);
        };
        Check(both(20.0, [&] { return a.joined && b.joined && a.BlockAt({ 8, -1, 8 }) != BlockType::COUNT && b.BlockAt({ 8, -1, 8 }) != BlockType::COUNT; }),
              "two creative players join an empty world");
        server.QueueConsoleCommand("tp Builder 21.5 0 17.5");
        server.QueueConsoleCommand("tp Watcher 19.5 0 17.5");
        both(20.0, [&] { return a.feet.x > 21.0f && b.feet.x > 19.0f && b.feet.x < 20.0f; });

        std::cout << "Structures" << std::endl;
        // A blast furnace (as FactoryTest builds it), formed by the builder.
        server.QueueConsoleCommand("fill 20 0 20 22 2 22 fire_brick");
        fill({ 21, 0, 20 }, { 21, 0, 19 }, "air_pipe");
        fill({ 21, 1, 20 }, { 21, 1, 20 }, "item_pipe");
        fill({ 21, 1, 19 }, { 21, 1, 19 }, "chest");
        fill({ 21, 1, 21 }, { 21, 1, 21 }, "smelter");
        fill({ 21, 2, 21 }, { 21, 2, 21 }, "air"); // its hollow top, left empty
        both(10.0, [&] { return a.BlockAt({ 21, 1, 21 }) == BlockType::SMELTER && a.BlockAt({ 21, 2, 21 }) == BlockType::AIR && b.BlockAt({ 21, 1, 19 }) == BlockType::CHEST; });
        a.held = ItemType::HAMMER;
        wait(0.3);
        a.client.UseBed({ 22, 2, 22 });
        Check(both(5.0, [&] { return Has(a.client, "Formed a Blast furnace"); }), "the builder forms a blast furnace");
        Check(both(5.0, [&] { return b.BlockAt({ 21, 1, 21 }) == BlockType::MULTIBLOCK && b.BlockAt({ 20, 0, 20 }) == BlockType::MULTIBLOCK_PART; }),
              "and the watcher sees it formed");
        a.held = BlockType::AIR;

        std::cout << "Chests" << std::endl;
        server.QueueConsoleCommand("give sand 3 Builder");
        both(5.0, [&] { return a.client.GetResources(BlockType::SAND) >= 3 * Crafting::kBlock; });
        a.client.OpenChest({ 21, 1, 19 });
        both(5.0, [&] { return a.client.GetOpenChest() == glm::ivec3(21, 1, 19); });
        a.client.MoveChestItems(BlockType::SAND, 3, true);
        both(5.0, [&] { return a.client.GetChestUnits(BlockType::SAND) == 3 * Crafting::kBlock; });
        b.client.OpenChest({ 21, 1, 19 });
        Check(both(5.0, [&] { return b.client.GetOpenChest() == glm::ivec3(21, 1, 19) && b.client.GetChestUnits(BlockType::SAND) == 3 * Crafting::kBlock; }),
              "what the builder puts in a chest, the watcher finds there");
        b.client.MoveChestItems(BlockType::SAND, 1, false);
        Check(both(5.0, [&] { return b.client.GetResources(BlockType::SAND) == Crafting::kBlock && a.client.GetChestUnits(BlockType::SAND) == 2 * Crafting::kBlock; }),
              "and what the watcher takes out is gone for the builder too (both have it open)");
        a.client.CloseChest();
        b.client.CloseChest();
        b.client.BreakBlock({ 22, 2, 22 });
        Check(both(5.0, [&] { return a.BlockAt({ 21, 1, 21 }) == BlockType::SMELTER && a.BlockAt({ 20, 0, 20 }) == BlockType::FIRE_BRICK; }),
              "the watcher breaks a part: the builder sees it taken apart");

        std::cout << "Fluids" << std::endl;
        fill({ 26, 0, 20 }, { 26, 0, 20 }, "fluid_tank");
        both(5.0, [&] { return a.BlockAt({ 26, 0, 20 }) == BlockType::FLUID_TANK; });
        server.QueueConsoleCommand("fluid 26 0 20 crude");
        auto tankSeen = [&](TestClient& c) {
            for (const auto& m : c.client.GetMachines())
                if (m.pos == glm::ivec3(26, 0, 20)) return m.depth == static_cast<uint8_t>(Fluids::Kind::Crude) && m.pressure > 0;
            return false;
        };
        Check(both(10.0, [&] { return tankSeen(a) && tankSeen(b); }), "a tank filled with crude shows it to both");

        std::cout << "Miners" << std::endl;
        // A mining post with iron ore near it, paid in bread by the builder.
        const glm::ivec3 post{ 30, 0, 14 };
        fill(post, post, "mining_post");
        fill({ 33, -2, 14 }, { 33, -2, 16 }, "iron_ore");
        both(10.0, [&] { return a.BlockAt(post) == BlockType::MINING_POST && a.BlockAt({ 33, -2, 16 }) == BlockType::IRON_ORE; });
        server.QueueConsoleCommand("tp Builder 28.5 0 14.5");
        both(10.0, [&] { return a.feet.x > 28.0f; });
        server.QueueConsoleCommand("give bread 2 Builder");
        both(5.0, [&] { return a.client.GetResources(ItemType::BREAD) >= 2 * Crafting::kBlock; });
        a.client.OpenChest(post);
        both(5.0, [&] { return a.client.GetOpenChest() == post; });
        a.client.MoveChestItems(ItemType::BREAD, 2, true);
        a.client.CloseChest();
        Check(both(15.0, [&] { return a.Find(CreatureType::DwarfMiner) && b.Find(CreatureType::DwarfMiner); }), "the miners a post hires turn up for both players");
        Check(both(20.0, [&] { const auto m = b.Find(CreatureType::DwarfMiner); return m && m->action == 2; }), "and the watcher sees them dig (working)");
        b.client.BreakBlock(post);
        both(10.0, [&] { return !a.Find(CreatureType::DwarfMiner); });

        std::cout << "The king and the market" << std::endl;
        server.QueueConsoleCommand("tp Builder 21.5 0 17.5");
        both(10.0, [&] { return a.feet.x < 22.0f; });
        server.QueueConsoleCommand("summon dwarf_king 23.5 0 17.5");
        Check(both(10.0, [&] { return a.Find(CreatureType::DwarfKing) && b.Find(CreatureType::DwarfKing); }), "both see a Dwarf King");
        const uint16_t king = a.Find(CreatureType::DwarfKing)->id;
        a.client.Attack(king, Tool::Sword);
        Check(both(10.0, [&] { const auto k = b.Find(CreatureType::DwarfKing); return k && k->health < 0.99f; }), "the builder's hit shows on his health for the watcher");
        server.QueueConsoleCommand("creatures clear");
        both(10.0, [&] { return a.client.GetCreatures().empty(); });

        server.QueueConsoleCommand("summon dwarf_trader 22.5 0 15.5");
        server.QueueConsoleCommand("give gold_ingot 3 Builder");
        both(10.0, [&] { return a.Find(CreatureType::DwarfTrader) && a.client.GetResources(ItemType::GOLD_INGOT) >= 3 * Crafting::kBlock; });
        a.client.Trade(a.Find(CreatureType::DwarfTrader)->id, static_cast<uint8_t>(Trades::Offers().size() - 1));
        Check(both(10.0, [&] { const auto g = b.Find(CreatureType::DwarfGuard); return g && g->tame; }), "a guard the builder hires wears the band for the watcher too");
        server.QueueConsoleCommand("tp Builder 40.5 0 17.5");
        Check(both(20.0, [&] {
                  const auto g = b.Find(CreatureType::DwarfGuard);
                  return g && glm::distance(glm::vec2(g->feet.x, g->feet.z), glm::vec2(40.5f, 17.5f)) < 5.0f;
              }),
              "and follows the builder, not the watcher");

        a.client.Disconnect();
        b.client.Disconnect();
        wait(0.3);
        server.Stop();
    }
    fs::remove_all(dir);
    std::cout << (failures ? "SYNC TEST FAILED (" : "ALL SYNC CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
