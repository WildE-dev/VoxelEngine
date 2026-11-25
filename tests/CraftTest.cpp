// Crafting (survival). Checks that
//  - every recipe makes sense (takes something, makes something), and glass lets light
//    through and doesn't hide what's behind it,
//  - inventories (world/Inventory.h): stacks fill up and spill into empty slots, part
//    blocks come out of (and go back into) the type's offcut, a full one takes no more,
//  - over a real connection: a recipe turns what you carry into its output, as many
//    times as asked; one that can't be afforded changes nothing; tools are items (a
//    second can be made); creative players don't craft (they have everything),
//  - upgrades work on the tool in hand: stone to iron, then iron to steel, and steel
//    tools beat iron,
//  - tools made are saved with the player.
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <thread>

#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "world/Crafting.h"
#include "world/Smelting.h"

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

Server::Options MakeOptions(const fs::path& dir) {
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = std::getenv("CRAFT_LOG") != nullptr;
    options.worldDirectory = dir.string();
    options.worldType = TerrainGenerator::Type::Empty;
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Survival;
    return options;
}

} // namespace

int main() {
    using Crafting::kBlock;
    std::cout << "Items" << std::endl;
    {
        // Every item type has an id that names it (and no block has it), and its file in
        // Resources/Data/items.
        bool named = true;
        for (int t = 1; t < static_cast<int>(ItemType::COUNT); t++) {
            const ItemType type = static_cast<ItemType>(t);
            const ItemId item = Items::Of(type);
            named &= Items::FromId(ItemTypeId(type)) == item && BlockFromId(ItemTypeId(type)) == BlockType::COUNT && Items::IsItem(item) &&
                     !Items::IsBlock(item) && Items::OfKind(Items::KindOf(item)) == item && Items::Valid(item);
        }
        Check(named, "every item type is an item by its id, not a block");
        std::string error;
        bool files = ItemData::Load(ItemData::kPath, error);
        for (int t = 1; files && t < static_cast<int>(ItemType::COUNT); t++) files &= ItemData::Texture(static_cast<ItemType>(t)).has_value();
        Check(files, "Resources/Data/items reads cleanly, with every item's icon" + (error.empty() ? "" : " (" + error + ")"));
        Check(Items::Name(Items::Of(ItemType::IRON_INGOT)) == "Iron ingot" && Items::MaxStack(Items::Of(ItemType::BUCKET)) == 1 &&
                  Items::MaxStack(Items::Of(ItemType::IRON_INGOT)) == Items::kMaxStack,
              "an iron ingot is called that and stacks; a bucket doesn't");
    }
    std::cout << "Recipes" << std::endl;
    bool sensible = true;
    for (const auto& r : Crafting::Recipes()) {
        sensible &= !r.inputs.empty() && (r.MakesGear() ? r.output == Items::kNone : r.outputUnits > 0 && Items::IsStackable(r.output));
        for (const auto& in : r.inputs) sensible &= in.units > 0 && Items::IsStackable(in.item);
    }
    Check(sensible && Crafting::Recipes().size() >= 10, "every recipe takes something and makes something");
    Check(Crafting::Find("planks") >= 0 && Crafting::Find("pickaxe") >= 0 && Crafting::Find("cake") < 0, "recipes are found by id");
    {
        // Every signal block is wired with copper.
        bool copper = true;
        for (const char* id : { "lever", "pressure_plate", "logic_gate", "signal_lamp", "water_sensor", "pressure_sensor", "daylight_sensor", "player_sensor",
                                "machine_sensor" }) {
            const int r = Crafting::Find(id);
            bool needs = false;
            if (r >= 0)
                for (const auto& in : Crafting::Recipes()[r].inputs) needs |= in.item == ItemType::COPPER_INGOT;
            if (!needs) std::cout << "        " << id << " takes no copper" << std::endl;
            copper = copper && needs;
        }
        Check(copper, "every signal block takes copper");
        std::array<uint32_t, Items::kKinds> noCopper{};
        noCopper[Items::KindOf(Items::Of(BlockType::STONE))] = noCopper[Items::KindOf(Items::Of(BlockType::GLASS))] = 8 * Block::kUnitsPerBlock;
        auto withCopper = noCopper;
        withCopper[Items::KindOf(Items::Of(ItemType::COPPER_INGOT))] = Block::kUnitsPerBlock;
        const auto& gate = Crafting::Recipes()[Crafting::Find("logic_gate")];
        Check(Crafting::TimesAffordable(gate, noCopper) == 0 && Crafting::TimesAffordable(gate, withCopper) == 2, "a logic gate can't be made without copper (half an ingot each)");
        Check(Smelting::FirstAffordable(std::array<uint32_t, Items::kKinds>{}) == nullptr, "(nothing to smelt in an empty chest)");
        // Limestone to cement (smelted, or burnt by hand with coal), and cement to concrete.
        std::array<uint32_t, Items::kKinds> quarry{};
        quarry[Items::KindOf(Items::Of(BlockType::LIMESTONE))] = 4 * Block::kUnitsPerBlock;
        quarry[Items::KindOf(Items::Of(ItemType::COAL))] = Block::kUnitsPerBlock;
        const SmeltRecipe* burnt = Smelting::FirstAffordable(quarry);
        Check(burnt && burnt->input == BlockType::LIMESTONE && burnt->output == ItemType::CEMENT && burnt->outputUnits == Block::kUnitsPerBlock,
              "a smelter burns limestone into cement");
        Check(Crafting::TimesAffordable(Crafting::Recipes()[Crafting::Find("cement")], quarry) == 2, "and by hand, two limestone and half a coal make a cement");
        std::array<uint32_t, Items::kKinds> mix{};
        mix[Items::KindOf(Items::Of(ItemType::CEMENT))] = Block::kUnitsPerBlock;
        mix[Items::KindOf(Items::Of(BlockType::GRAVEL))] = 2 * Block::kUnitsPerBlock;
        mix[Items::KindOf(Items::Of(BlockType::SAND))] = Block::kUnitsPerBlock;
        const auto& concrete = Crafting::Recipes()[Crafting::Find("concrete")];
        Check(Crafting::TimesAffordable(concrete, mix) == 1 && concrete.output == BlockType::CONCRETE && concrete.outputUnits == 4 * Block::kUnitsPerBlock &&
                  Crafting::TimesAffordable(Crafting::Recipes()[Crafting::Find("asphalt")], mix) == 0,
              "a cement, two gravel and a sand make four concrete");
    }
    std::array<uint32_t, Items::kKinds> held{};
    held[Items::KindOf(Items::Of(BlockType::LOG))] = 3 * kBlock;
    Check(Crafting::TimesAffordable(Crafting::Recipes()[Crafting::Find("planks")], held) == 3, "three logs make planks three times");
    {
        std::cout << "Inventories" << std::endl;
        Inventory inv(3);
        const ItemId stone = Items::OfBlock(BlockType::STONE);
        Check(inv.Add(stone, 100) == 0 && inv.slots[0].count == 64 && inv.slots[1].count == 36 && inv.Count(stone) == 100, "100 stone fill a stack of 64 and spill into the next slot");
        Check(inv.Add(Items::OfTool(Tool::Pickaxe), 2) == 1 && inv.Count(Items::OfTool(Tool::Pickaxe)) == 1, "tools don't stack: one pickaxe fits in the last slot");
        Check(inv.Room(stone) == 28 && inv.Add(stone, 30) == 2, "28 more stone fit, and no more");
        Check(inv.TakeUnits(BlockType::STONE, kBlock / 4) && inv.Count(stone) == 127 && inv.offcuts[Items::KindOf(Items::Of(BlockType::STONE))] == kBlock * 3 / 4 &&
                  inv.Units(BlockType::STONE) == 128 * kBlock - kBlock / 4,
              "a quarter block of stone cuts one, leaving three quarters as an offcut");
        Check(inv.TakeUnits(BlockType::STONE, kBlock / 2) && inv.Count(stone) == 127 && inv.offcuts[Items::KindOf(Items::Of(BlockType::STONE))] == kBlock / 4,
              "the next half comes from the offcut");
        Check(inv.AddUnits(BlockType::STONE, kBlock * 3 / 4) == 0 && inv.Count(stone) == 128 && inv.offcuts[Items::KindOf(Items::Of(BlockType::STONE))] == 0,
              "three quarters back make it whole again");
        Check(!inv.TakeUnits(BlockType::STONE, 200 * kBlock) && inv.Count(stone) == 128, "taking more than there is takes nothing");
        Check(!inv.FitsUnits(BlockType::DIRT, kBlock) && inv.FitsUnits(BlockType::DIRT, kBlock / 2) && inv.FitsUnits(BlockType::STONE, 0),
              "a full inventory takes no whole block of something new (but an offcut, yes)");
        Check(Items::FromId("iron_pickaxe") == Items::OfTool(Tool::Pickaxe, 1) && Items::FromId("steel_chisel") == Items::OfTool(Tool::Chisel, 2) &&
                  Items::FromId("cap") == Items::OfArmour(ArmourPiece::Cap) && Items::FromId("stone") == stone && Items::FromId("cake") == Items::kNone &&
                  Items::Id(Items::OfTool(Tool::Axe, 2)) == "steel_axe",
              "items by id: blocks, tools of each tier, armour");
        Check(inv.BestTier(Tool::Pickaxe) == 0 && inv.BestTier(Tool::Axe) == -1, "the best tool of a kind carried");
    }
    Check(!Block(BlockType::GLASS).BlocksLight() && !Block(BlockType::GLASS).HidesNeighbours() && Block(BlockType::GLASS).IsFullBlock() &&
              Block(BlockType::STONE).HidesNeighbours(),
          "glass is solid but lets light through and shows what's behind it");

    std::random_device rd;
    const fs::path dir = fs::temp_directory_path() / ("voxel-craft-test-" + std::to_string(rd()));
    fs::remove_all(dir);
    {
        Server server;
        if (!server.Start(MakeOptions(dir))) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Crafter", error, kTestViewDistance);
        auto pump = [&](double s, const std::function<bool()>& done) { return Pump(a, s, done); };
        auto wait = [&](double s) { pump(s, [] { return false; }); };
        Check(pump(20.0, [&] { return a.joined; }), "a survival player joins");
        // On the ground (survival players build within 6 blocks).
        server.QueueConsoleCommand("tp Crafter 7.5 0 7.5");
        pump(20.0, [&] { return a.feet.y < 0.5f; });
        auto units = [&](ItemRef t) { return a.client.GetResources(static_cast<ItemId>(t)); };
        // The client shows crafts at once (predicted); this waits until the server has
        // really done them: a command sent after them is answered after them (one
        // ordered channel), so once its reply is in, so are they.
        auto sync = [&] {
            auto replies = [&] {
                size_t n = 0;
                for (const auto& line : a.client.GetChat()) n += line.text.find("You're in") != std::string::npos;
                return n;
            };
            const size_t before = replies();
            a.client.SendChat("/gamemode");
            pump(20.0, [&] { return replies() > before; });
            wait(0.2); // (and its Resources answer)
        };
        Check(!a.client.HasTool(Tool::Pickaxe) && !a.client.CanCraft(Crafting::Find("planks")), "with nothing: no tools, nothing to craft");

        std::cout << "Crafting" << std::endl;
        server.QueueConsoleCommand("give log 2 Crafter");
        server.QueueConsoleCommand("give stone 3 Crafter");
        server.QueueConsoleCommand("give sand 1 Crafter");
        Check(pump(20.0, [&] { return units(BlockType::LOG) == 2 * kBlock && units(BlockType::STONE) == 3 * kBlock && units(BlockType::SAND) == kBlock; }),
              "given 2 logs, 3 stone and a sand");
        a.client.Craft(Crafting::Find("planks"), 2);
        sync();
        Check(pump(20.0, [&] { return units(BlockType::PLANKS) == 8 * kBlock && units(BlockType::LOG) == 0; }), "2 logs make 8 planks");
        a.client.Craft(Crafting::Find("planks"), 1); // (the client refuses; the server would too)
        wait(0.5);
        Check(units(BlockType::PLANKS) == 8 * kBlock, "with no logs left, no more planks");
        a.client.Craft(Crafting::Find("pickaxe"), 1);
        sync();
        Check(pump(20.0, [&] { return a.client.HasTool(Tool::Pickaxe); }) && units(BlockType::PLANKS) == 7 * kBlock &&
                  units(BlockType::STONE) == 3 * kBlock - kBlock * 3 / 2,
              "a pickaxe takes a plank and 1.5 stone, and is kept");
        Check(a.client.GetCount(Items::OfTool(Tool::Pickaxe)) == 1 && a.client.ToolTier(Tool::Pickaxe) == 0 && a.client.CanCraft(Crafting::Find("pickaxe")),
              "it's a stone pickaxe in a slot (and a second could be made)");
        a.client.Craft(Crafting::Find("glass"), 1);
        sync();
        Check(pump(20.0, [&] { return units(BlockType::GLASS) == kBlock && units(BlockType::SAND) == 0; }), "sand makes glass");
        // (Once the ground there has arrived: the server has it loaded too.)
        pump(20.0, [&] { Block b; return a.world.GetBlock(9, -1, 9, b) && !b.IsAir(); });
        a.client.PlaceBlock({ 9, 0, 9 }, BlockType::GLASS);
        sync();
        const bool built = pump(20.0, [&] { Block b; return a.world.GetBlock(9, 0, 9, b) && b.type == BlockType::GLASS; });
        Block at;
        a.world.GetBlock(9, 0, 9, at);
        Check(built && units(BlockType::GLASS) == 0, "and the glass can be built with (block " + std::string(BlockId(at.type)) + ", glass " +
                                                         std::to_string(units(BlockType::GLASS)) + ", sand " + std::to_string(units(BlockType::SAND)) + ")");
        a.client.Craft(Crafting::Find("lamp"), 1);
        sync();
        Check(units(BlockType::LAMP) == 0 && units(BlockType::PLANKS) == 7 * kBlock, "a lamp needs glass, which is used up: nothing happens");

        std::cout << "Swords and armour" << std::endl;
        server.QueueConsoleCommand("give stone 2 Crafter");
        server.QueueConsoleCommand("give hide 8 Crafter");
        sync();
        a.client.Craft(Crafting::Find("sword"), 1);
        sync();
        Check(pump(20.0, [&] { return a.client.HasTool(Tool::Sword); }) && units(BlockType::PLANKS) == 7 * kBlock - kBlock / 2,
              "half a plank and 2 stone make a sword");
        for (const char* piece : { "cap", "tunic", "boots" }) a.client.Craft(Crafting::Find(piece), 1);
        sync();
        Check(pump(20.0, [&] { return a.client.HasArmour(ArmourPiece::Cap) && a.client.HasArmour(ArmourPiece::Tunic) && a.client.HasArmour(ArmourPiece::Boots); }) &&
                  units(ItemType::HIDE) == 0,
              "8 hide make a cap, a tunic and boots, worn at once");
        Check(std::abs(Armour::Protection(a.client.GetArmour()) - 0.48f) < 1e-4f && a.client.GetCount(Items::OfArmour(ArmourPiece::Cap)) == 0,
              "they go into the armour slots, and together take off 48%");
        Check(Armour::Reduce(7, a.client.GetArmour()) == 4 && Armour::Reduce(1, a.client.GetArmour()) == 1 && Armour::Reduce(4, 0) == 4,
              "a sword's 7 comes down to 4; a hit always does at least one");
        // (Planks were used for the sword: 6.5 left.)

        std::cout << "Iron and steel" << std::endl;
        server.QueueConsoleCommand("give steel_ingot 3 Crafter");
        server.QueueConsoleCommand("give iron_ingot 3 Crafter");
        sync();
        pump(20.0, [&] { return units(ItemType::STEEL_INGOT) == 3 * kBlock && units(ItemType::IRON_INGOT) == 3 * kBlock; });
        const int iron = Crafting::Find("iron_tools"), steel = Crafting::Find("steel_tools");
        Check(!a.client.CanCraft(steel, 1, Tool::Pickaxe) && !a.client.CanCraft(iron, 1, Tool::Axe) && !a.client.CanCraft(iron, 1, Tool::None) &&
                  a.client.CanCraft(iron, 1, Tool::Pickaxe),
              "an upgrade needs the tool in hand, of the tier below: no steel before iron, nothing for a tool not carried");
        a.client.Craft(iron, 1, Tool::Pickaxe);
        sync();
        Check(pump(20.0, [&] { return a.client.ToolTier(Tool::Pickaxe) == 1; }) && units(ItemType::IRON_INGOT) == 2 * kBlock && a.client.ToolTier(Tool::Sword) == 0 &&
                  a.client.GetCount(Items::OfTool(Tool::Pickaxe)) == 0,
              "an iron ingot makes the stone pickaxe in hand iron (the sword stays stone)");
        Check(a.client.CanCraft(steel, 1, Tool::Pickaxe), "after which it can be made steel");
        a.client.Craft(steel, 1, Tool::Pickaxe);
        sync();
        Check(pump(20.0, [&] { return a.client.ToolTier(Tool::Pickaxe) == 2; }) && units(ItemType::STEEL_INGOT) == 2 * kBlock &&
                  a.client.GetCount(Items::OfTool(Tool::Pickaxe, 2)) == 1,
              "and a steel ingot makes it steel");
        const Block stone(BlockType::STONE);
        Check(Mining::Seconds(stone, Tool::Pickaxe, 2) < Mining::Seconds(stone, Tool::Pickaxe, 1) &&
                  Mining::Seconds(stone, Tool::Pickaxe, 1) < Mining::Seconds(stone, Tool::Pickaxe, 0) && Combat::Damage(Tool::Sword, 2) > Combat::Damage(Tool::Sword, 1),
              "steel tools mine faster than iron ones, and a steel sword hits harder");

        server.QueueConsoleCommand("gamemode creative Crafter");
        pump(20.0, [&] { return a.client.IsCreative(); });
        Check(a.client.HasTool(Tool::Axe) && !a.client.CanCraft(Crafting::Find("stone_brick")), "creative players have every tool and craft nothing");
        server.QueueConsoleCommand("gamemode survival Crafter");
        pump(20.0, [&] { return !a.client.IsCreative(); });
        Check(a.client.HasTool(Tool::Pickaxe) && !a.client.HasTool(Tool::Axe), "back in survival, only the pickaxe they made");
        a.client.Disconnect();
        Pump(a, 0.3, [] { return false; });
        server.Stop();
    }
    {
        std::cout << "Saving" << std::endl;
        Server server;
        if (!server.Start(MakeOptions(dir))) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Crafter", error, kTestViewDistance);
        const bool back = Pump(a, 20.0, [&] { return a.joined && a.client.GetResources(BlockType::PLANKS) > 0; });
        Check(back && a.client.HasTool(Tool::Pickaxe) && !a.client.HasTool(Tool::Shovel) && a.client.GetResources(BlockType::PLANKS) == 7 * kBlock - kBlock / 2,
              "after a restart: the pickaxe and the planks are still theirs (joined " + std::to_string(a.joined) + ", mode " +
                  std::to_string(static_cast<int>(a.client.GetMode())) + ", planks " + std::to_string(a.client.GetResources(BlockType::PLANKS)) +
                  ", pickaxe " + std::to_string(a.client.HasTool(Tool::Pickaxe)) + ")");
        Check(a.client.HasTool(Tool::Sword) && a.client.HasArmour(ArmourPiece::Tunic), "and so are the sword and the armour");
        Check(a.client.ToolTier(Tool::Pickaxe) == 2 && a.client.ToolTier(Tool::Sword) == 0, "and the pickaxe is still steel");
        a.client.Disconnect();
        Pump(a, 0.3, [] { return false; });
        server.Stop();
    }
    fs::remove_all(dir);
    std::cout << (failures ? "CRAFT TEST FAILED (" : "ALL CRAFT CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
