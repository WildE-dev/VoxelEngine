// World saving, without windows or GL. Uses a fresh directory under the system temp
// directory and checks that
//  - edits survive SaveAll + Flush and a restart, and untouched chunks aren't stored,
//  - edited chunks are saved when they unload, without an explicit SaveAll,
//  - stored chunks above a column report it as blocked for the sky light estimate,
//    and light under a saved roof comes out right after a restart,
//  - corrupt or newer-format files are refused instead of silently dropped,
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

    std::cout << "Players" << std::endl;
    glm::vec3 leftAt(0.0f);
    {
        auto server = StartServer(dir);
        Check(server != nullptr, "a server starts on the saved world");
        if (!server) return EXIT_FAILURE;
        TestClient alice;
        std::string error;
        Check(alice.client.Connect("127.0.0.1", server->GetPort(), "Alice", error), "Alice connects");
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
        Check(alice.client.Connect("127.0.0.1", server->GetPort(), "Alice", error), "Alice reconnects");
        Check(Pump(alice, 10.0, [&] { return alice.client.IsWelcomed(); }), "Alice is welcomed");
        Check(alice.client.GetSpawnFlags() & net::kWelcomeResume, "Alice is resumed");
        Check(glm::distance(alice.client.GetSpawn(), leftAt) < 0.01f, "at the position she left from");
        Check(std::abs(alice.client.GetSpawnYaw() - 123.0f) < 0.01f && std::abs(alice.client.GetSpawnPitch() + 20.0f) < 0.01f,
              "facing the same way");
        Check(Pump(alice, 60.0, [&] { return alice.player.IsSpawned(); }) && glm::distance(alice.player.GetFeetPosition(), leftAt) < 0.05f,
              "and lands exactly there");

        Check(bob.client.Connect("127.0.0.1", server->GetPort(), "Bob", error), "Bob connects");
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
