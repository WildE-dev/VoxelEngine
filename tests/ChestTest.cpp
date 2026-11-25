// Chests over real connections. Checks that
//  - a chest is crafted from planks and placed like any block,
//  - right-clicking (OpenChest) opens it in reach, not from afar,
//  - things moved in leave the player and show in the chest, and another player who
//    opens it sees them and can take them out,
//  - you can't move more than you have, or into a chest you haven't opened,
//  - stacks move between slots (yours and the chest's), swap with another kind, and
//    Shift-click across; an armour slot takes only its own piece,
//  - breaking a chest gives its contents to whoever broke it, and closes it for
//    anyone who had it open,
//  - a chest and what's in it are still there after a restart.
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
#include "world/Inventory.h"
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

bool Has(const TestClient& c, const std::string& text) {
    for (const auto& line : c.client.GetChat())
        if (line.text.find(text) != std::string::npos) return true;
    return false;
}

Server::Options MakeOptions(const fs::path& dir) {
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = std::getenv("CHEST_LOG") != nullptr;
    options.worldDirectory = dir.string();
    options.worldType = TerrainGenerator::Type::Empty;
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Survival;
    return options;
}

} // namespace

int main() {
    constexpr uint32_t kBlock = Block::kUnitsPerBlock;
    const fs::path dir = fs::temp_directory_path() / ("voxel_chest_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const glm::ivec3 chestAt{ 9, 0, 7 }; // beside the spawn point (7.5, 0, 7.5) in an empty world
    {
        Server server;
        if (!server.Start(MakeOptions(dir))) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a, b;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Keeper", error, kTestViewDistance);
        b.client.Connect("127.0.0.1", server.GetPort(), "Visitor", error, kTestViewDistance);
        auto both = [&](double s, const std::function<bool()>& done) { return Pump({ &a, &b }, s, done); };
        auto wait = [&](double s) { both(s, [] { return false; }); };
        int syncs = 0;
        auto sync = [&] {
            const std::string word = "sync" + std::to_string(++syncs);
            server.QueueConsoleCommand("msg Keeper " + word);
            return both(20.0, [&] { return Has(a, word); });
        };
        auto ground = [](TestClient& c) { Block g; return c.world.GetBlock(9, -1, 7, g) && !g.IsAir(); };
        Check(both(20.0, [&] { return a.joined && b.joined && ground(a) && ground(b); }), "two survival players join");
        // On the ground beside the chest's spot (they'd stay floating where they joined).
        server.QueueConsoleCommand("tp Keeper 7.5 0 7.5");
        server.QueueConsoleCommand("tp Visitor 11.5 0 7.5");
        both(20.0, [&] { return a.feet.y < 0.5f && b.feet.y < 0.5f; });

        std::cout << "Making and placing" << std::endl;
        server.QueueConsoleCommand("give planks 2 Keeper");
        sync();
        a.client.Craft(Crafting::Find("chest"), 1);
        Check(both(20.0, [&] { return a.client.GetResources(BlockType::CHEST) == kBlock && a.client.GetResources(BlockType::PLANKS) == 0; }),
              "2 planks make a chest");
        a.client.PlaceBlock(chestAt, BlockType::CHEST);
        Check(both(20.0, [&] { Block c; return b.world.GetBlock(chestAt.x, chestAt.y, chestAt.z, c) && c.type == BlockType::CHEST; }),
              "it's placed, and the other player sees it");

        std::cout << "Storing" << std::endl;
        a.client.MoveChestItems(BlockType::STONE, 1, true);
        wait(0.5);
        Check(!a.client.GetOpenChest(), "nothing goes into a chest that isn't open");
        server.QueueConsoleCommand("give stone 5 Keeper");
        server.QueueConsoleCommand("give cooked_venison 2 Keeper");
        sync();
        a.client.OpenChest(chestAt);
        Check(both(20.0, [&] { return a.client.GetOpenChest() == chestAt; }), "right-clicking opens it");
        a.client.MoveChestItems(BlockType::STONE, 3, true);
        a.client.MoveChestItems(ItemType::COOKED_VENISON, 2, true);
        Check(both(20.0, [&] { return a.client.GetChestUnits(BlockType::STONE) == 3 * kBlock && a.client.GetChestUnits(ItemType::COOKED_VENISON) == 2 * kBlock; }) &&
                  a.client.GetResources(BlockType::STONE) == 2 * kBlock && a.client.GetResources(ItemType::COOKED_VENISON) == 0,
              "3 stone and 2 cooked venison go in, and leave the player");
        a.client.MoveChestItems(BlockType::STONE, 10, true);
        Check(both(20.0, [&] { return a.client.GetChestUnits(BlockType::STONE) == 5 * kBlock; }) && a.client.GetResources(BlockType::STONE) == 0,
              "asking to put in 10 stone puts in the 2 they had left");

        std::cout << "Sharing" << std::endl;
        b.client.OpenChest(chestAt);
        Check(both(20.0, [&] { return b.client.GetOpenChest() == chestAt && b.client.GetChestUnits(BlockType::STONE) == 5 * kBlock; }),
              "the other player opens it and sees the same things");
        b.client.MoveChestItems(ItemType::COOKED_VENISON, 1, false);
        Check(both(20.0, [&] { return b.client.GetResources(ItemType::COOKED_VENISON) == kBlock && a.client.GetChestUnits(ItemType::COOKED_VENISON) == kBlock; }),
              "and takes a cooked venison out (the keeper's open chest shows it gone)");

        std::cout << "Slots" << std::endl;
        // Where a kind of item is (-1: nowhere).
        auto slotOf = [](const Inventory& inv, ItemId item) {
            for (size_t i = 0; i < inv.slots.size(); i++)
                if (!inv.slots[i].Empty() && inv.slots[i].item == item) return static_cast<int>(i);
            return -1;
        };
        const ItemId venison = Items::Of(ItemType::COOKED_VENISON), stone = Items::OfBlock(BlockType::STONE);
        b.client.MoveItems(net::Box::Player, slotOf(b.client.GetInventory(), venison), net::Box::Player, 15);
        Check(both(20.0, [&] { return slotOf(b.client.GetInventory(), venison) == 15; }), "a stack moves from one slot to another");
        b.client.MoveItems(net::Box::Chest, slotOf(b.client.GetChest(), stone), net::Box::Player, net::kAnySlot);
        Check(both(20.0, [&] { return b.client.GetCount(stone) == 5 && a.client.GetChestUnits(BlockType::STONE) == 0 && slotOf(b.client.GetInventory(), stone) == 0; }),
              "Shift-click: the chest's stone comes over, into the first free slot (the hotbar's)");
        b.client.MoveItems(net::Box::Player, 0, net::Box::Chest, 7, 2);
        Check(both(20.0, [&] { return b.client.GetCount(stone) == 3 && b.client.GetChest().slots[7] == Stack{ stone, 2 }; }), "two of the five go into slot 8 of the chest");
        b.client.MoveItems(net::Box::Player, 0, net::Box::Player, 15);
        Check(both(20.0, [&] { return b.client.GetInventory().slots[15] == Stack{ stone, 3 } && b.client.GetInventory().slots[0] == Stack{ venison, 1 }; }),
              "a stack put on another kind swaps with it");
        b.client.MoveItems(net::Box::Player, 0, net::Box::Player, net::kAnySlot);
        Check(both(20.0, [&] { return slotOf(b.client.GetInventory(), venison) == 10; }), "Shift-click in the hotbar moves it to the rest of the inventory");
        b.client.MoveItems(net::Box::Player, 15, net::Box::Chest, net::kAnySlot);
        Check(both(20.0, [&] { return b.client.GetCount(stone) == 0 && a.client.GetChestUnits(BlockType::STONE) == 5 * kBlock; }), "and the stone all goes back in");
        server.QueueConsoleCommand("give cap 2 Visitor");
        Check(both(20.0, [&] { return b.client.HasArmour(ArmourPiece::Cap) && b.client.GetCount(Items::OfArmour(ArmourPiece::Cap)) == 1; }),
              "given two caps: one goes on, the other into a slot");
        const int cap = slotOf(b.client.GetInventory(), Items::OfArmour(ArmourPiece::Cap));
        b.client.MoveItems(net::Box::Player, cap, net::Box::Armour, static_cast<int>(ArmourPiece::Tunic));
        b.client.MoveItems(net::Box::Armour, static_cast<int>(ArmourPiece::Cap), net::Box::Player, net::kAnySlot);
        Check(both(20.0, [&] { return !b.client.HasArmour(ArmourPiece::Cap) && b.client.GetCount(Items::OfArmour(ArmourPiece::Cap)) == 2; }) &&
                  !b.client.HasArmour(ArmourPiece::Tunic),
              "a cap won't go in the tunic's slot; taken off, both caps are in slots");
        b.client.MoveItems(net::Box::Player, cap, net::Box::Armour, net::kAnySlot);
        Check(both(20.0, [&] { return b.client.HasArmour(ArmourPiece::Cap) && b.client.GetCount(Items::OfArmour(ArmourPiece::Cap)) == 1; }), "and one goes back on");
        b.client.CloseChest();
        server.QueueConsoleCommand("tp Visitor 40 0 40");
        both(20.0, [&] { return b.feet.x > 39.0f; });
        b.client.OpenChest(chestAt);
        wait(1.0);
        Check(!b.client.GetOpenChest(), "from 30 blocks away it won't open");

        std::cout << "Signs" << std::endl;
        const glm::ivec3 signAt{ 8, 0, 9 };
        server.QueueConsoleCommand("give sign 1 Keeper");
        sync();
        a.client.PlaceBlock(signAt, BlockType::SIGN);
        both(20.0, [&] { Block s; return a.world.GetBlock(signAt.x, signAt.y, signAt.z, s) && s.type == BlockType::SIGN; });
        a.client.WriteSign(signAt, "Keep out!\x01 (mine)");
        Check(both(20.0, [&] { auto it = b.client.GetSigns().find(signAt); return it != b.client.GetSigns().end() && it->second == "Keep out! (mine)"; }),
              "words written on a sign reach the other player (unprintable ones dropped)");
        a.client.WriteSign({ 30, 0, 30 }, "nothing here");
        both(1.0, [] { return false; });
        Check(a.client.GetSigns().size() == 1, "and there's no writing where there's no sign");

        std::cout << "Saving" << std::endl;
        a.client.CloseChest();
        server.QueueConsoleCommand("save");
        sync();
        a.client.Disconnect();
        b.client.Disconnect();
        Pump({ &a, &b }, 0.3, [] { return false; });
        server.Stop();
    }
    {
        Server server;
        if (!server.Start(MakeOptions(dir))) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Keeper", error, kTestViewDistance);
        auto pump = [&](double s, const std::function<bool()>& done) { return Pump({ &a }, s, done); };
        pump(20.0, [&] { return a.joined; });
        server.QueueConsoleCommand("tp Keeper 7.5 0 7.5");
        pump(20.0, [&] { return a.feet.y < 0.5f; });
        Check(pump(20.0, [&] { Block c; return a.joined && a.world.GetBlock(chestAt.x, chestAt.y, chestAt.z, c) && c.type == BlockType::CHEST; }),
              "after a restart the chest is still there");
        a.client.OpenChest(chestAt);
        Check(pump(20.0, [&] { return a.client.GetChestUnits(BlockType::STONE) == 5 * kBlock && a.client.GetChestUnits(ItemType::COOKED_VENISON) == kBlock; }),
              "with the same things in it");

        Check(pump(20.0, [&] { auto it = a.client.GetSigns().find({ 8, 0, 9 }); return it != a.client.GetSigns().end() && it->second == "Keep out! (mine)"; }),
              "the sign's words are still there after a restart");

        std::cout << "Breaking" << std::endl;
        server.QueueConsoleCommand("gamemode creative Keeper"); // (breaks at once)
        pump(20.0, [&] { return a.client.IsCreative(); });
        a.client.BreakBlock(chestAt);
        Check(pump(20.0, [&] { return !a.client.GetOpenChest(); }), "breaking it closes it");
        Check(pump(20.0, [&] { return a.client.GetResources(BlockType::STONE) == 5 * kBlock && a.client.GetResources(ItemType::COOKED_VENISON) == kBlock; }),
              "and what was in it goes to whoever broke it");
        a.client.Disconnect();
        Pump({ &a }, 0.3, [] { return false; });
        server.Stop();
    }
    fs::remove_all(dir);
    std::cout << (failures ? "CHEST TEST FAILED (" : "ALL CHEST CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
