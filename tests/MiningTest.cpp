// Mining (survival). Checks the rules (world/Mining.h): how long each kind of block
// takes by hand, that the right tool is four times faster and the wrong one no
// faster, that carved blocks take less, and plants none; and over a real connection
// that the server
//  - refuses a break that wasn't mined, or wasn't mined for long enough,
//  - lets it through once it has been, by hand or faster with the right tool,
//  - holds the tool to what it was started with,
//  - picks pebbles up at once, as a quarter block of stone,
//  - and in creative breaks at once.
// And dropped items (server/DroppedItems.h): they fall and land, slide farther on ice,
// join up lying together; what a block leaves flies to whoever broke it; a thrown
// stack lies where it lands until someone walks over it.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/DroppedItems.h"
#include "server/Server.h"
#include "world/Crafting.h"
#include "world/Creatures.h"
#include "world/Mining.h"
#include "world/TerrainGenerator.h"

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
    std::cout << "Dropped items" << std::endl;
    {
        struct Floor : BlockAccess {
            BlockType floor = BlockType::STONE;
            bool GetBlock(int, int y, int, Block& block) const override {
                block = y < 0 ? Block(floor) : Block();
                return true;
            }
        };
        Floor stone;
        DroppedItems items;
        const uint32_t id = items.Drop(Items::OfBlock(BlockType::DIRT), 3, 0, { 0.5f, 3.0f, 0.5f }, glm::vec3(0.0f), 0.0f);
        for (int i = 0; i < 60; i++) items.Update(stone, 0.05f);
        Check(id && items.Count() == 1 && items.All()[0].onGround && std::abs(items.All()[0].position.y - DroppedItems::Half()) < 1e-3f,
              "an item falls and lies on the ground");
        items.Drop(Items::OfBlock(BlockType::DIRT), 2, 32, { 0.8f, 0.5f, 0.6f }, glm::vec3(0.0f), 0.0f);
        items.Drop(Items::OfBlock(BlockType::SAND), 1, 0, { 0.6f, 0.5f, 0.4f }, glm::vec3(0.0f), 0.0f);
        for (int i = 0; i < 30; i++) items.Update(stone, 0.05f);
        uint32_t dirt = 0, dirtUnits = 0;
        for (const auto& d : items.All())
            if (d.item == Items::OfBlock(BlockType::DIRT)) dirt += d.count, dirtUnits += d.units;
        Check(items.Count() == 2 && dirt == 5 && dirtUnits == 32, "stacks of the same thing lying together join up (and others don't)");
        auto slide = [](BlockType floor) {
            Floor f;
            f.floor = floor;
            DroppedItems one;
            one.Drop(Items::OfBlock(BlockType::DIRT), 1, 0, { 0.5f, DroppedItems::Half(), 0.5f }, { 4.0f, 0.0f, 0.0f }, 0.0f);
            for (int i = 0; i < 100; i++) one.Update(f, 0.05f);
            return one.All()[0].position.x - 0.5f;
        };
        const float onStone = slide(BlockType::STONE), onIce = slide(BlockType::ICE);
        Check(onStone < 1.0f && onIce > 3.0f, "it slides farther on ice (" + std::to_string(onIce) + " blocks, on stone " + std::to_string(onStone) + ")");
        DroppedItems old;
        old.Drop(Items::OfBlock(BlockType::DIRT), 1, 0, { 0.5f, DroppedItems::Half(), 0.5f }, glm::vec3(0.0f), 0.0f);
        for (int i = 0; i < 6100; i++) old.Update(stone, 0.05f);
        Check(old.Count() == 0, "and it's gone after five minutes");
    }
    std::cout << "Ores and iron" << std::endl;
    {
        // Ores in the stone under a normal world: coal from 8 blocks down, iron (rarer)
        // from 24 down.
        TerrainGenerator terrain;
        int coal = 0, iron = 0, shallowOre = 0, stone = 0;
        for (int cx = 0; cx < 3; cx++)
            for (int cz = 0; cz < 3; cz++) {
                TerrainGenerator::ColumnHeights heights;
                terrain.ComputeColumnHeights(cx, cz, heights);
                for (int cy = -5; cy <= 4; cy++) {
                    TerrainGenerator::Blocks blocks;
                    terrain.GenerateChunk({ cx, cy, cz }, heights, blocks);
                    for (int x = 0; x < Chunk::CHUNK_SIZE; x++)
                        for (int z = 0; z < Chunk::CHUNK_SIZE; z++) {
                            const int surface = terrain.ColumnAt(cx * Chunk::CHUNK_SIZE + x, cz * Chunk::CHUNK_SIZE + z).surfaceRow;
                            for (int y = 0; y < Chunk::CHUNK_SIZE; y++) {
                                const BlockType t = blocks[Chunk::Index(x, y, z)].type;
                                const int depth = surface - (cy * Chunk::CHUNK_SIZE + y);
                                coal += t == BlockType::COAL_ORE;
                                iron += t == BlockType::IRON_ORE;
                                stone += t == BlockType::STONE;
                                shallowOre += (t == BlockType::COAL_ORE && depth < 8) || (t == BlockType::IRON_ORE && depth < 24);
                            }
                        }
                }
            }
        Check(coal > 0 && iron > 0 && iron < coal && coal * 10 < stone, "coal and (rarer) iron ore lie in the stone (" + std::to_string(coal) + " coal, " +
                                                                              std::to_string(iron) + " iron, " + std::to_string(stone) + " stone)");
        Check(shallowOre == 0, "coal no shallower than 8 blocks, iron no shallower than 24");
        Check(ResourceTypeOf(BlockType::COAL_ORE) == BlockType::COAL && IsItemType(BlockType::COAL) && IsItemType(BlockType::IRON_INGOT),
              "coal ore breaks into coal; coal and ingots are items");
        Check(Seconds(Block(BlockType::IRON_ORE), Tool::Pickaxe, true) < Seconds(Block(BlockType::IRON_ORE), Tool::Pickaxe) &&
                  Combat::Damage(Tool::Sword, true) == Combat::Damage(Tool::Sword) + 2 && Combat::Damage(Tool::None, true) == Combat::Damage(Tool::None),
              "the iron upgrade mines faster and hits a heart harder (not bare-handed)");
        Check(Crafting::Find("iron_ingot") >= 0 && Crafting::Recipes()[Crafting::Find("iron_tools")].ironUpgrade, "iron ore smelts into ingots, which make the upgrade");
    }

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
        if (type == BlockType::WATER || type == BlockType::LAVA || IsPlantType(type)) continue; // (liquids aren't mined)
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
    // On the ground beside the row (survival players reach 6 blocks).
    server.QueueConsoleCommand("tp Miner 11.5 0 9.5");
    both(20.0, [&] { return std::abs(a.feet.y) < 0.1f; });
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
    Check(both(20.0, [&] { return TypeAt(builder, { 8, -1, 8 }) == BlockType::AIR; }) &&
              both(20.0, [&] { return a.client.GetResources(BlockType::GRASS) == 64; }),
          "a shovel takes grass in a moment, and the block's worth flies to the miner");

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

    std::cout << "Reach" << std::endl;
    builder.client.PlaceBlock({ 25, 0, 9 }, BlockType::PEBBLES); // 13 blocks from the miner
    Check(both(20.0, [&] { return TypeAt(a, { 25, 0, 9 }) == BlockType::PEBBLES; }), "pebbles 13 blocks off");
    a.client.BreakBlock({ 25, 0, 9 });
    wait(1.0);
    Check(TypeAt(builder, { 25, 0, 9 }) == BlockType::PEBBLES, "are out of a survival player's reach (6 blocks)");
    builder.client.BreakBlock({ 25, 0, 9 });
    Check(both(20.0, [&] { return TypeAt(a, { 25, 0, 9 }) == BlockType::AIR; }), "but not a creative player's");

    std::cout << "Creative" << std::endl;
    builder.client.BreakBlock({ 13, 0, 12 });
    Check(both(20.0, [&] { return TypeAt(a, { 13, 0, 12 }) == BlockType::AIR; }), "in creative a block breaks at once");

    std::cout << "Felling trees" << std::endl;
    {
        // Two trunks five high with a crown of leaves on each; and a third tree off to the
        // side whose leaves must stay.
        for (const int x : { 30, 38 }) {
            server.QueueConsoleCommand("fill " + std::to_string(x - 2) + " 3 28 " + std::to_string(x + 2) + " 5 32 leaves");
            server.QueueConsoleCommand("fill " + std::to_string(x) + " 0 30 " + std::to_string(x) + " 4 30 log");
        }
        server.QueueConsoleCommand("fill 46 3 28 50 5 32 leaves");
        server.QueueConsoleCommand("fill 48 0 30 48 4 30 log");
        Check(both(20.0, [&] { return TypeAt(builder, { 48, 4, 30 }) == BlockType::LOG && TypeAt(builder, { 30, 5, 30 }) == BlockType::LEAVES; }), "three trees stand");
        // A leaf the builder put down by the tree, and a flower on the grass nearby.
        builder.client.PlaceBlock({ 32, 1, 30 }, BlockType::LEAVES);
        server.QueueConsoleCommand("fill 34 0 34 34 0 34 flower_red");
        both(10.0, [&] { return TypeAt(a, { 32, 1, 30 }) == BlockType::LEAVES && TypeAt(a, { 34, 0, 34 }) == BlockType::FLOWER_RED; });
        builder.client.BreakBlock({ 30, 0, 30 }, Tool::Axe);
        bool felled = both(20.0, [&] {
            for (int y = 0; y <= 4; y++)
                if (TypeAt(a, { 30, y, 30 }) != BlockType::AIR) return false;
            return true;
        });
        Check(felled, "breaking the bottom log with an axe fells the whole trunk");
        builder.client.BreakBlock({ 38, 0, 30 }, Tool::Pickaxe);
        Check(both(20.0, [&] { return TypeAt(a, { 38, 0, 30 }) == BlockType::AIR; }) && TypeAt(a, { 38, 1, 30 }) == BlockType::LOG,
              "with any other tool, only the one log goes");
        auto leavesLeft = [&](int x0) {
            int n = 0;
            for (int x = x0 - 2; x <= x0 + 2; x++)
                for (int y = 3; y <= 5; y++)
                    for (int z = 28; z <= 32; z++) n += TypeAt(a, { x, y, z }) == BlockType::LEAVES ? 1 : 0;
            return n;
        };
        wait(1.0);
        Check(leavesLeft(30) > 20, "the felled tree's leaves don't all go at once");
        Check(both(15.0, [&] { return leavesLeft(30) == 0; }), "but they all fall away within a few seconds");
        Check(leavesLeft(38) == 73 && leavesLeft(48) == 73, "leaves a log still holds up stay");
        Check(TypeAt(a, { 32, 1, 30 }) == BlockType::LEAVES, "and so do leaves a player put down, with no log near");
        builder.client.BreakBlock({ 34, -1, 34 });
        Check(both(10.0, [&] {
                  for (const auto& i : a.client.GetItems())
                      if (i.item == Items::OfBlock(BlockType::FLOWER_RED)) return TypeAt(a, { 34, 0, 34 }) == BlockType::AIR;
                  return false;
              }),
              "a flower whose ground goes drops as an item");
    }

    std::cout << "Throwing items away" << std::endl;
    {
        int slot = -1;
        const auto& slots = a.client.GetInventory().slots;
        for (size_t i = 0; i < slots.size(); i++)
            if (!slots[i].Empty() && slots[i].item == Items::OfBlock(BlockType::GRASS)) slot = static_cast<int>(i);
        a.client.DropItem(net::Box::Player, slot, 1, glm::vec3(1.0f, 0.0f, 0.0f));
        Check(slot >= 0 && both(20.0, [&] { return a.client.GetResources(BlockType::GRASS) == 0; }), "the miner throws their grass away");
        // (Only the grass: the felled tree's leaves may have dropped a sapling or two.)
        auto grassOn = [](const Client& c) {
            std::vector<net::ItemInfo> out;
            for (const auto& i : c.GetItems())
                if (i.item == Items::OfBlock(BlockType::GRASS)) out.push_back(i);
            return out;
        };
        Check(both(20.0, [&] { return grassOn(a.client).size() == 1 && grassOn(builder.client).size() == 1; }), "and everyone sees it lying there");
        wait(2.5);
        const auto lying = grassOn(a.client);
        Check(lying.size() == 1 && a.client.GetResources(BlockType::GRASS) == 0 && lying[0].position.x > 12.5f && std::abs(lying[0].position.y - DroppedItems::Half()) < 0.05f,
              "it flies off ahead and lands on the ground (not straight back to them)");
        if (!lying.empty()) {
            char tp[96];
            std::snprintf(tp, sizeof tp, "tp Miner %.2f 0 %.2f", lying[0].position.x, lying[0].position.z);
            server.QueueConsoleCommand(tp);
        }
        Check(both(20.0, [&] { return a.client.GetResources(BlockType::GRASS) == 64 && grassOn(a.client).empty(); }), "walking over it picks it up again");
        // Thrown as hard as it goes (the throw held right down), it goes much further.
        const float tossed = lying.empty() ? 0.0f : lying[0].position.x - 11.5f;
        const float from = a.feet.x;
        a.client.DropItem(net::Box::Player, slot, 1, glm::vec3(1.0f, 0.0f, 0.0f), 1.0f);
        both(20.0, [&] { return grassOn(a.client).size() == 1; });
        wait(3.0);
        const auto far = grassOn(a.client);
        Check(far.size() == 1 && far[0].position.x - from > tossed + 3.0f,
              "held right down, a throw goes much further (" + std::to_string(far.empty() ? 0.0f : far[0].position.x - from) + " blocks, a toss " + std::to_string(tossed) + ")");
    }

    std::cout << "Cheating" << std::endl;
    {
        // The server holds survival players to how they can move; creative ones may fly.
        server.QueueConsoleCommand("tp Miner 11.5 0 9.5");
        both(10.0, [&] { return std::abs(a.feet.x - 11.5f) < 0.1f && std::abs(a.feet.y) < 0.1f; });
        wait(0.5);
        a.feet.x += 15.0f; // (in one update)
        Check(both(5.0, [&] { return a.feet.x < 12.0f; }), "running 15 blocks in a twentieth of a second is put back");
        wait(0.5);
        a.feet.y += 5.0f; // (straight up, in the air)
        Check(both(5.0, [&] { return a.feet.y < 0.5f; }), "rising five blocks into the air is put back down");
        wait(0.5);
        a.feet.y += 1.0f; // (a jump's height, but staying up there)
        const glm::vec3 up = a.feet;
        Check(both(6.0, [&] { return a.feet.y < 0.5f; }), "hanging a block up in the air (not coming down) is put back down");
        (void)up;
        wait(0.5);
        a.feet.x += 0.2f; // (an ordinary step)
        wait(1.0);
        Check(a.feet.x > 11.6f, "an ordinary step isn't");
        const float builderY = builder.feet.y;
        for (int i = 0; i < 16; i++) { // (flying up, half a block an update)
            builder.feet.y += 0.5f;
            wait(0.06);
        }
        wait(2.5);
        Check(builder.feet.y > builderY + 7.0f, "a creative player may fly up and stay there");
    }

    a.client.Disconnect();
    builder.client.Disconnect();
    Pump({ &a, &builder }, 0.3, [] { return false; });
    server.Stop();
    std::cout << (failures ? "MINING TEST FAILED (" : "ALL MINING CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
