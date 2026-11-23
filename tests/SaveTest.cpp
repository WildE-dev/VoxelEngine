// World saving, without windows or GL. Uses a fresh directory under the system temp
// directory and checks that
//  - edits survive SaveAll + Flush and a restart, and untouched chunks aren't stored,
//  - edited chunks are saved when they unload, without an explicit SaveAll,
//  - stored chunks above a column report it as blocked for the sky light estimate,
//    and light under a saved roof comes out right after a restart,
//  - corrupt or newer-format files are refused instead of silently dropped,
//  - a world saved before inventories (format 13) loads with each player's units,
//    tools (at their upgrade's tier) and armour, and each chest's units, as stacks,
//    and saves and loads again as they are,
//  - a player who rejoins a saved server by name gets their position and look back,
//    and lands exactly there; a new player doesn't.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "app/Player.h"
#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "server/ServerWorld.h"
#include "server/WorldStorage.h"

namespace fs = std::filesystem;

// Test clients ask for a short view (chunks): plenty for the checks, and light enough
// to run every suite at once.
constexpr int kTestViewDistance = 4;

namespace {

using Clock = std::chrono::steady_clock;

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

bool WaitFor(double seconds, const std::function<bool()>& done) {
    const auto deadline = Clock::now() + std::chrono::duration<double>(seconds);
    while (Clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

// Waits until every chunk in the column x/z 0, chunk y from -2 to 4, is generated and lit.
bool WaitLoaded(ServerWorld& world) {
    return WaitFor(60.0, [&] {
        for (int y = -2; y <= 4; y++) {
            auto chunk = world.FindChunk({ 0, y, 0 });
            if (!chunk || !chunk->IsLoaded() || chunk->GetLightVersion() == 0) return false;
        }
        return true;
    });
}

bool BlockIs(const ServerWorld& world, const glm::ivec3& p, const Block& expected) {
    Block block;
    return world.GetBlock(p.x, p.y, p.z, block) && block == expected;
}

uint8_t SkyLightAt(const ServerWorld& world, const glm::ivec3& p) {
    auto chunk = world.FindChunk(Coords::ChunkOf(p));
    Chunk::Light light;
    chunk->CopyPublishedLight(light);
    const glm::ivec3 l = Coords::LocalOf(p);
    return light[Chunk::Index(l.x, l.y, l.z)] >> 4;
}

// Edits, high above the terrain (chunk y 3 is rows 48..63) and below it.
const glm::ivec3 kStonePos(3, 50, 3);
const glm::ivec3 kWaterPos(6, 52, 6);
const glm::ivec3 kSecondPos(9, 55, 12);
const Block kWater = Block::Water(77);

struct TestClient {
    ClientWorld world;
    Client client{ world };
    Player player{ glm::vec3(0.0f) };
    bool joined = false;
    float yaw = 0.0f, pitch = 0.0f;

    void Update(float dt) {
        client.Poll();
        if (!joined && client.IsWelcomed()) {
            joined = true;
            player.Teleport(client.GetSpawn());
            player.ResetSpawn();
            if (client.GetSpawnFlags() & net::kWelcomeResume) {
                player.ResumeAt(client.GetSpawn());
                yaw = client.GetSpawnYaw();
                pitch = client.GetSpawnPitch();
            }
        }
        if (!joined) return;
        if (!player.IsSpawned()) player.Spawn(world);
        player.Update(world, PlayerInput{}, dt);
        world.Update(player.GetEyePosition());
        if (player.IsSpawned()) client.SendPlayerState(player.GetFeetPosition(), player.GetVelocity(), yaw, pitch, 0);
    }
};

bool Pump(TestClient& c, double seconds, const std::function<bool()>& done) {
    const auto deadline = Clock::now() + std::chrono::duration<double>(seconds);
    auto last = Clock::now();
    while (Clock::now() < deadline) {
        const auto now = Clock::now();
        c.Update(std::chrono::duration<float>(now - last).count());
        last = now;
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

std::unique_ptr<Server> StartServer(const fs::path& dir) {
    auto server = std::make_unique<Server>();
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = false;
    options.worldDirectory = dir.string();
    options.autosaveSeconds = 0.0f;
    if (!server->Start(options)) return nullptr;
    server->RunInBackground();
    return server;
}

} // namespace

int main() {
    std::random_device rd;
    const fs::path dir = fs::temp_directory_path() / ("voxel-save-test-" + std::to_string(rd()));
    fs::remove_all(dir);
    TerrainGenerator terrain;

    std::cout << "World data" << std::endl;
    {
        WorldStorage storage(dir);
        Check(storage.Open(), "a new world directory opens");
        ServerWorld world(terrain, &storage);
        world.Update({ glm::ivec3(0) });
        Check(WaitLoaded(world), "chunks generate");
        world.SetBlock(kStonePos.x, kStonePos.y, kStonePos.z, BlockType::STONE);
        world.SetBlock(kWaterPos.x, kWaterPos.y, kWaterPos.z, kWater);
        Check(world.SaveAll() == 1, "SaveAll stores exactly the one edited chunk");
        Check(world.SaveAll() == 0, "a second SaveAll has nothing new to store");
        storage.SetFluids({ { glm::ivec3(4, 5, 6), 2, 1500 }, { glm::ivec3(-7, 8, 9), 3, 64 } });
        Check(storage.Flush(), "flush writes the save");
    }
    Check(fs::exists(dir / "world.dat") && fs::exists(dir / "regions" / "r.0.0.0.vxr"), "world.dat and the region file exist");
    bool leftovers = false;
    for (const auto& e : fs::recursive_directory_iterator(dir)) leftovers |= e.path().extension() == ".tmp";
    Check(!leftovers, "no temporary files are left behind");

    {
        WorldStorage storage(dir);
        Check(storage.Open(), "the saved world opens again");
        Check(storage.GetStoredChunkCount() == 1, "only the edited chunk is stored");
        const auto fluids = storage.GetFluids();
        Check(fluids.size() == 2 && fluids[0].pos == glm::ivec3(4, 5, 6) && fluids[0].kind == 2 && fluids[0].units == 1500 && fluids[1].kind == 3 &&
                  fluids[1].units == 64,
              "what fluid pipes and tanks hold is saved with the world");

        WorldStorage::ColumnMask mask;
        storage.SkyBlockedAbove({ 0, 0, 0 }, mask);
        Check(WorldStorage::IsColumnSet(mask, 3, 3) && WorldStorage::IsColumnSet(mask, 6, 6), "columns under the stone and the water are blocked");
        Check(!WorldStorage::IsColumnSet(mask, 10, 10), "an open column isn't");
        storage.SkyBlockedAbove({ 0, 3, 0 }, mask);
        Check(!WorldStorage::IsColumnSet(mask, 3, 3), "a chunk isn't blocked by itself");

        ServerWorld world(terrain, &storage);
        world.Update({ glm::ivec3(0) });
        Check(WaitLoaded(world), "chunks load");
        Check(BlockIs(world, kStonePos, Block(BlockType::STONE)), "the placed stone is back");
        Check(BlockIs(world, kWaterPos, kWater), "the water is back with its volume");
        Check(WaitFor(10.0, [&] { return SkyLightAt(world, kStonePos - glm::ivec3(0, 1, 0)) == kMaxLight - 1; }) &&
                  SkyLightAt(world, glm::ivec3(10, 49, 10)) == kMaxLight,
              "sky light is shaded under the saved stone and full beside it");

        // Edit again, then walk away: the chunk must be saved as it unloads.
        world.SetBlock(kSecondPos.x, kSecondPos.y, kSecondPos.z, BlockType::LOG);
        world.Update({ glm::ivec3(40, 0, 0) });
        Check(storage.HasChunk(Coords::ChunkOf(kSecondPos)) && world.FindChunk({ 0, 3, 0 }) == nullptr, "an edited chunk is stored as it unloads");
        Check(storage.Flush(), "flush after unloading");
    }
    {
        WorldStorage storage(dir);
        Check(storage.Open(), "reopens after the unload save");
        ServerWorld world(terrain, &storage);
        world.Update({ glm::ivec3(0) });
        Check(WaitLoaded(world), "chunks load");
        Check(BlockIs(world, kSecondPos, Block(BlockType::LOG)) && BlockIs(world, kStonePos, Block(BlockType::STONE)),
              "both the unload-saved edit and the earlier one are there");
    }
    {
        // A saved stone roof (bottom layer of chunk y 6) over the chunk at y 5, with
        // the roof's chunk out of vertical range: the chunk below must not get full sky
        // light from the generator's estimate.
        const fs::path roofDir = dir.string() + "-roof";
        WorldStorage storage(roofDir);
        Check(storage.Open(), "roof world opens");
        auto roof = std::make_unique<Chunk::Blocks>();
        for (int x = 0; x < Chunk::CHUNK_SIZE; x++)
            for (int z = 0; z < Chunk::CHUNK_SIZE; z++) (*roof)[Chunk::Index(x, 0, z)] = Block(BlockType::STONE);
        storage.StoreChunk({ 0, 6, 0 }, *roof);
        ServerWorld world(terrain, &storage);
        world.Update({ glm::ivec3(0, -6, 0) }); // loads chunk y -18..5
        Check(WaitFor(60.0, [&] {
                  auto c = world.FindChunk({ 0, 5, 0 });
                  return c && c->IsLoaded() && c->GetLightVersion() > 0;
              }) && world.FindChunk({ 0, 6, 0 }) == nullptr,
              "the chunk under the roof loads, the roof's chunk doesn't");
        const uint8_t underRoof = SkyLightAt(world, { 8, 95, 8 });
        Check(underRoof < kMaxLight - 4, "sky light under the middle of an unloaded saved roof is shaded (" + std::to_string(underRoof) + ")");
        fs::remove_all(roofDir);
    }

    std::cout << "Damaged saves" << std::endl;
    {
        const fs::path bad = dir.string() + "-bad";
        fs::create_directories(bad / "regions");
        const char truncated[] = "VXRG\x01\0\0\0\x05\0\0\0garbage";
        std::ofstream(bad / "regions" / "r.0.0.0.vxr", std::ios::binary) << std::string(truncated, sizeof truncated - 1);
        WorldStorage storage(bad);
        Check(!storage.Open(), "a truncated region file is refused");
        fs::remove_all(bad);

        fs::create_directories(bad);
        std::ofstream(bad / "world.dat", std::ios::binary) << std::string("VXWD\x63\0\0\0", 8);
        WorldStorage newer(bad);
        Check(!newer.Open(), "a world from a newer format is refused");
        fs::remove_all(bad);
    }

    std::cout << "Old saves" << std::endl;
    {
        const fs::path old = dir.string() + "-old";
        fs::create_directories(old);
        const auto stone = static_cast<size_t>(BlockType::STONE), planks = static_cast<size_t>(BlockType::PLANKS), coal = static_cast<size_t>(BlockType::COAL);
        net::ByteWriter w;
        for (char c : { 'V', 'X', 'W', 'D' }) w.U8(static_cast<uint8_t>(c));
        w.U32(13);  // format
        w.U8(0);    // world type
        w.F32(0.3f); // time of day
        w.U8(0);    // default mode: survival
        w.VarU32(1); // one player
        w.Str("Old");
        w.Vec3(glm::vec3(3.0f, 10.0f, 4.0f));
        w.F32(0.0f), w.F32(0.0f), w.U8(0);
        w.U8(0); // survival
        w.VarU32(2); // units of two types: 100.25 stone, 3 planks
        w.U8(static_cast<uint8_t>(stone)), w.VarU32(100 * 64 + 16);
        w.U8(static_cast<uint8_t>(planks)), w.VarU32(3 * 64);
        w.U8((1 << 1) | (1 << 2) | (1 << 7)); // a pickaxe, an axe, and the iron upgrade
        w.VarU32(2);
        w.U8(3), w.VarU32(1), w.U8(static_cast<uint8_t>(Armour::Bit(ArmourPiece::Cap))); // armour: a cap
        w.U8(6), w.VarU32(1), w.U8(1);                                                   // more tools: the chisel
        w.VarU32(1); // one chest: 5.5 coal
        w.IVec3(glm::ivec3(1, 2, 3));
        w.VarU32(1);
        w.U8(static_cast<uint8_t>(coal)), w.VarU32(5 * 64 + 32);
        for (int i = 0; i < 5; i++) w.VarU32(0); // no crops, signs, signals, fluids or air
        std::ofstream(old / "world.dat", std::ios::binary).write(reinterpret_cast<const char*>(w.data.data()), static_cast<std::streamsize>(w.data.size()));

        auto check = [&](const char* when) {
            WorldStorage storage(old);
            const bool opened = storage.Open();
            const auto p = opened ? storage.FindPlayer("Old") : std::nullopt;
            const auto chests = opened ? storage.GetChests() : WorldStorage::Chests{};
            Check(p && p->inventory.Count(Items::OfBlock(BlockType::STONE)) == 100 && p->inventory.offcuts[stone] == 16 &&
                      p->inventory.Units(BlockType::PLANKS) == 3 * 64 && p->inventory.slots.size() == Inventory::kPlayerSlots,
                  std::string(when) + ": the old player's 100.25 stone and 3 planks are stacks (and a quarter offcut)");
            Check(p && p->inventory.Count(Items::OfTool(Tool::Pickaxe, 1)) == 1 && p->inventory.Count(Items::OfTool(Tool::Axe, 1)) == 1 &&
                      p->inventory.Count(Items::OfTool(Tool::Chisel, 1)) == 1 && p->inventory.BestTier(Tool::Shovel) < 0 &&
                      p->armour == Armour::Bit(ArmourPiece::Cap),
                  std::string(when) + ": their pickaxe, axe and chisel are iron (they had the upgrade), and the cap is still worn");
            Check(chests.size() == 1 && chests[0].second.Count(Items::OfBlock(BlockType::COAL)) == 5 && chests[0].second.offcuts[coal] == 32,
                  std::string(when) + ": the chest's 5.5 coal are 5 coal and half an offcut");
            if (p && opened && std::string(when) == "converted") {
                storage.StorePlayer("Old", *p);
                storage.SetChests(chests);
                Check(storage.Flush(), "saved again in the new format");
            }
        };
        check("converted");
        check("saved again");
        fs::remove_all(old);
    }

    std::cout << "Players" << std::endl;
    glm::vec3 leftAt(0.0f);
    {
        auto server = StartServer(dir);
        Check(server != nullptr, "a server starts on the saved world");
        if (!server) return EXIT_FAILURE;
        TestClient alice;
        std::string error;
        Check(alice.client.Connect("127.0.0.1", server->GetPort(), "Alice", error, kTestViewDistance), "Alice connects");
        Check(Pump(alice, 60.0, [&] { return alice.player.IsSpawned(); }), "Alice lands on the terrain");
        Check(!(alice.client.GetSpawnFlags() & net::kWelcomeResume), "a new player isn't resumed");
        alice.yaw = 123.0f;
        alice.pitch = -20.0f;
        Pump(alice, 1.0, [] { return false; });
        leftAt = alice.player.GetFeetPosition();
        alice.client.Disconnect();
        server->Stop(); // saves
    }
    {
        auto server = StartServer(dir);
        if (!server) return EXIT_FAILURE;
        TestClient alice, bob;
        std::string error;
        Check(alice.client.Connect("127.0.0.1", server->GetPort(), "Alice", error, kTestViewDistance), "Alice reconnects");
        Check(Pump(alice, 10.0, [&] { return alice.client.IsWelcomed(); }), "Alice is welcomed");
        Check(alice.client.GetSpawnFlags() & net::kWelcomeResume, "Alice is resumed");
        Check(glm::distance(alice.client.GetSpawn(), leftAt) < 0.01f, "at the position she left from");
        Check(std::abs(alice.client.GetSpawnYaw() - 123.0f) < 0.01f && std::abs(alice.client.GetSpawnPitch() + 20.0f) < 0.01f,
              "facing the same way");
        Check(Pump(alice, 60.0, [&] { return alice.player.IsSpawned(); }) && glm::distance(alice.player.GetFeetPosition(), leftAt) < 0.05f,
              "and lands exactly there");

        Check(bob.client.Connect("127.0.0.1", server->GetPort(), "Bob", error, kTestViewDistance), "Bob connects");
        Check(Pump(bob, 10.0, [&] { return bob.client.IsWelcomed(); }) && !(bob.client.GetSpawnFlags() & net::kWelcomeResume),
              "Bob, never seen before, isn't resumed");
        alice.client.Disconnect();
        bob.client.Disconnect();
        server->Stop();
    }

    fs::remove_all(dir);
    std::cout << (failures ? "SAVE TEST FAILED (" : "ALL SAVE CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
