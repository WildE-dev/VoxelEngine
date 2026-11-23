// Game modes: creative (anything, unhurt) and survival (blocks cost resources that
// breaking blocks gives, and traps hurt). Checks that
//  - a block's worth follows its shape: 64 for a full block, 32 for a half slab, its
//    volume for a chiselled one; plants and machines are worth a full block,
//  - and over real connections, in survival:
//    - breaking a block gives its worth, placing one (at a chosen height) costs it,
//    - what you can't afford isn't placed, and water can't be placed at all,
//    - chiselling off gives back what it takes, building up costs it,
//    - spikes hurt,
//  - in creative: blocks cost nothing and nothing hurts,
//  - /gamemode and /give are for operators (and the console),
//  - modes and resources are saved with the player, and a world keeps the default
//    mode it was made with.
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <thread>

#include "app/Player.h"
#include "world/BlockAccess.h"
#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "world/Collision.h"

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
    Player player{ glm::vec3(0.0f) };
    bool joined = false;

    void Update(float dt) {
        client.Poll();
        if (!joined && client.IsWelcomed()) {
            joined = true;
            player.Teleport(client.GetSpawn());
            player.ResetSpawn();
        }
        if (auto c = client.TakeCorrection()) {
            player.Teleport(*c);
            if (!player.IsFlying() && Collision::BoxBlocked(world, player.GetBounds())) player.ResetSpawn();
        }
        if (!joined) return;
        if (!player.IsSpawned()) player.Spawn(world);
        player.Update(world, PlayerInput{}, dt);
        world.Update(player.GetEyePosition());
        if (player.IsSpawned())
            client.SendPlayerState(player.GetFeetPosition(), player.GetVelocity(), 0.0f, 0.0f, player.IsOnGround() ? net::kFlagOnGround : 0);
    }
};

bool Pump(std::initializer_list<TestClient*> clients, double seconds, const std::function<bool()>& done) {
    auto last = std::chrono::steady_clock::now();
    const auto deadline = last + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        for (auto* c : clients) c->Update(dt);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

Block At(TestClient& c, const glm::ivec3& p) {
    Block b;
    if (!c.world.GetBlock(p.x, p.y, p.z, b)) return Block(BlockType::COUNT);
    return b;
}

bool Has(const TestClient& c, const std::string& text) {
    for (const auto& line : c.client.GetChat())
        if (line.text.find(text) != std::string::npos) return true;
    return false;
}

Server::Options ServerOptions(const fs::path& dir, net::GameMode mode) {
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = false;
    options.worldDirectory = dir.string();
    options.worldType = TerrainGenerator::Type::Empty; // flat ground: the top of row -1 is at 0
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = mode;
    // (Alice may die on the spikes; leaving things behind is TrapTest's and
    // CreatureTest's to check, and would muddle the counts checked here.)
    options.dropOnDeath = false;
    return options;
}

} // namespace

namespace {
// Deep ground for respawning: solid up to -151, air above to 0, nothing loaded beyond
// (or, with `loading`, nothing loaded at all yet).
struct DeepWorld : BlockAccess {
    bool loading = false;
    bool GetBlock(int, int y, int, Block& block) const override {
        if (loading || y < -220 || y > 0) return false;
        block = y <= -151 ? Block(BlockType::STONE) : Block();
        return true;
    }
};
} // namespace

// Flat ground (solid to y 0) with a wall at x 5 and up.
struct WalledWorld : BlockAccess {
    bool GetBlock(int x, int y, int, Block& block) const override {
        block = y < 0 || (x >= 5 && y < 6) ? Block(BlockType::STONE) : Block();
        return true;
    }
};

int main() {
    std::cout << "Creative flight" << std::endl;
    {
        WalledWorld walled;
        Player p(glm::vec3(0.5f, 3.0f, 0.5f));
        p.SetFlying(walled, true, true);
        PlayerInput in;
        in.lookDirection = glm::vec3(1.0f, 0.0f, 0.0f);
        in.yaw = 0.0f;
        in.move = glm::vec2(0.0f, 1.0f);
        for (int i = 0; i < 120; i++) p.Update(walled, in, 1.0f / 60.0f);
        Check(p.IsFlying() && p.GetFeetPosition().x < 5.0f && p.GetFeetPosition().x > 4.0f && std::abs(p.GetFeetPosition().y - 3.0f) < 0.01f,
              "flying (solid) toward a wall stops at it, hovering (x " + std::to_string(p.GetFeetPosition().x) + ")");
        in.move = glm::vec2(0.0f);
        in.jump = true;
        for (int i = 0; i < 60; i++) p.Update(walled, in, 1.0f / 60.0f);
        Check(p.GetFeetPosition().y > 8.0f, "jump held rises (y " + std::to_string(p.GetFeetPosition().y) + ")");
        in.jump = false;
        in.descend = true;
        for (int i = 0; i < 120 && p.IsFlying(); i++) p.Update(walled, in, 1.0f / 60.0f);
        Check(!p.IsFlying() && p.GetFeetPosition().y >= 0.0f && p.GetFeetPosition().y < 0.6f, "coming down onto the ground lands (walking again)");
    }
    std::cout << "Respawning far down" << std::endl;
    {
        DeepWorld deep;
        Player p(glm::vec3(0.5f, -150.0f, 0.5f));
        deep.loading = true;
        p.ResumeAt(glm::vec3(0.5f, -150.0f, 0.5f));
        Check(!p.Spawn(deep) && !p.IsSpawned(), "respawning at a bed far down (-150) waits while it isn't loaded");
        deep.loading = false;
        Check(p.Spawn(deep) && p.IsSpawned() && std::abs(p.GetFeetPosition().y + 150.0f) < 0.05f, "and once it is, stands there");
        Player q(glm::vec3(0.5f, -153.0f, 0.5f));
        q.ResumeAt(glm::vec3(0.5f, -153.0f, 0.5f)); // (in the rock)
        Check(q.Spawn(deep) && std::abs(q.GetFeetPosition().y + 150.0f) < 0.05f, "put in the rock there, it climbs out onto the floor above");
        Player r(glm::vec3(0.5f, -150.0f, 0.5f));
        r.ResetSpawn();
        Check(r.Spawn(deep) && std::abs(r.GetFeetPosition().y + 150.0f) < 0.05f, "and looking for the ground down its column finds it that far down too");
    }
    std::cout << "What blocks are worth" << std::endl;
    Check(Block(BlockType::STONE).ResourceUnits() == 64 && Block(BlockType::STONE_BRICK).ResourceUnits() == 64, "a full block is worth 64");
    Check(Block::Placed(BlockType::STONE_BRICK, 4).ResourceUnits() == 32 && Block::Placed(BlockType::DIRT, 4).ResourceUnits() == 32,
          "a half slab 32, stepped or sloped");
    Check(Block::Placed(BlockType::STONE_BRICK, 2).ResourceUnits() == 16 && Block::Placed(BlockType::STONE_BRICK, 6).ResourceUnits() == 48,
          "a quarter 16, three quarters 48");
    Check(Block(BlockType::STONE_BRICK).Chiselled(0, -2).ResourceUnits() == 60, "chiselling a quarter of a brick down by a quarter takes 4");
    {
        // A slope's worth is its volume: lowering any corner makes it worth less, and
        // raising it back restores it.
        const Block full(BlockType::DIRT);
        bool less = true, back = true;
        for (int e = 0; e < 4; e++) {
            const Block lowered = full.Chiselled(e, -2);
            less &= lowered.ResourceUnits() < 64;
            back &= lowered.Chiselled(e, 2).ResourceUnits() == 64;
        }
        Check(less && back, "a chiselled slope is worth less, and as much again once filled back in");
    }
    Check(Block::Placed(BlockType::PIPE, 4).ResourceUnits() == 64 && Block::Placed(BlockType::FLOWER_RED).ResourceUnits() == 64 &&
              Block::Placed(BlockType::PUMP, 4).ResourceUnits() == 64,
          "pipes, plants and pumps can't be shaped: always a full block's worth");
    Check(Block::Placed(BlockType::WATER).ResourceUnits() == 0 && Block().ResourceUnits() == 0, "water and air are worth nothing");

    std::random_device rd;
    const fs::path dir = fs::temp_directory_path() / ("voxel-mode-test-" + std::to_string(rd()));
    fs::remove_all(dir);

    std::cout << "Survival" << std::endl;
    auto server = std::make_unique<Server>();
    if (!server->Start(ServerOptions(dir, net::GameMode::Survival))) return EXIT_FAILURE;
    server->RunInBackground();
    auto a = std::make_unique<TestClient>(), b = std::make_unique<TestClient>();
    std::string error;
    a->client.Connect("127.0.0.1", server->GetPort(), "Alice", error, kTestViewDistance);
    b->client.Connect("127.0.0.1", server->GetPort(), "Bob", error, kTestViewDistance);
    auto both = [&](double s, const std::function<bool()>& done) { return Pump({ a.get(), b.get() }, s, done); };
    Check(both(30.0, [&] { return a->player.IsSpawned() && b->player.IsSpawned(); }), "two players join a new world");
    // Survival breaking takes time (world/Mining.h): mine, then break.
    // If the server was too busy to see the mining start in time, it refuses the break
    // and puts the block back: keep mining and try again, as the game does.
    auto mine = [&](const glm::ivec3& p) {
        const Block block = At(*a, p);
        a->client.StartMining(p, Tool::None);
        both(Mining::Seconds(block, Tool::None) + 0.1, [] { return false; });
        for (int attempt = 0; attempt < 10; attempt++) {
            a->client.BreakBlock(p);
            if (both(1.0, [&] { return At(*b, p).type != block.type; })) return;
            both(1.0, [&] { return At(*a, p).type == block.type; }); // put back: mine on
        }
    };
    Check(!a->client.IsCreative() && !b->client.IsCreative(), "in survival (the world's default)");
    Check(a->client.GetResources(BlockType::GRASS) == 0 && !a->client.CanAfford(BlockType::STONE, 8), "with nothing to build with");

    const glm::ivec3 ground(10, -1, 10);
    const BlockType groundType = At(*a, ground).type;
    mine(ground);
    Check(both(20.0, [&] { return At(*b, ground).IsAir() && a->client.GetResources(groundType) == 64; }), "breaking a block gives 64 of it");
    a->client.PlaceBlock(ground, groundType, 4);
    Check(both(20.0, [&] { return At(*b, ground).type == groundType && At(*b, ground).ResourceUnits() == 32; }) &&
              a->client.GetResources(groundType) == 32,
          "placing it as a half slab costs 32");
    const glm::ivec3 spot(12, 0, 10);
    a->client.PlaceBlock(spot, groundType, 8);
    both(1.0, [] { return false; });
    Check(At(*b, spot).IsAir() && a->client.GetResources(groundType) == 32, "a full block (64) can't be afforded with 32 left");
    a->client.PlaceBlock(spot, groundType, 2);
    Check(both(20.0, [&] { return At(*b, spot).type == groundType; }) && a->client.GetResources(groundType) == 16, "but a quarter (16) can");
    mine(spot);
    Check(both(20.0, [&] { return At(*b, spot).IsAir() && a->client.GetResources(groundType) == 32; }), "and breaking it gives the 16 back");

    a->client.SendChat("/give stone_brick 2");
    Check(both(20.0, [&] { return Has(*a, "Only operators can do that"); }) && a->client.GetResources(BlockType::STONE_BRICK) == 0, "players can't /give themselves blocks");
    server->QueueConsoleCommand("give stone_brick 1.5 Alice");
    Check(both(20.0, [&] { return a->client.GetResources(BlockType::STONE_BRICK) == 96 && Has(*a, "Server gave you 1.5 stone_brick"); }),
          "the console can (1.5 blocks: 96)");
    server->QueueConsoleCommand("give diamonds 3 Alice");
    const glm::ivec3 brick(11, 0, 8); // (within the 6 blocks a survival player reaches)
    a->client.PlaceBlock(brick, BlockType::STONE_BRICK);
    Check(both(20.0, [&] { return At(*b, brick).type == BlockType::STONE_BRICK && a->client.GetResources(BlockType::STONE_BRICK) == 32; }),
          "a full brick costs 64");
    a->client.Chisel(brick, 0, false);
    Pump({ a.get(), b.get() }, 0.5, [] { return false; });
    Check(At(*b, brick).IsFullBlock() && a->client.GetResources(BlockType::STONE_BRICK) == 32, "chiselling needs a chisel");
    server->QueueConsoleCommand("give chisel Alice");
    Check(both(20.0, [&] { return a->client.HasTool(Tool::Chisel); }), "given a chisel");
    a->client.Chisel(brick, 0, false);
    Check(both(20.0, [&] { return At(*b, brick).ResourceUnits() == 60; }) && a->client.GetResources(BlockType::STONE_BRICK) == 36,
          "chiselling it down gives back the 4 taken off");
    a->client.Chisel(brick, 0, true);
    Check(both(20.0, [&] { return At(*b, brick).IsFullBlock(); }) && a->client.GetResources(BlockType::STONE_BRICK) == 32, "building it back up costs 4");
    Check(!a->client.CanAfford(BlockType::WATER, 8), "water can't be placed in survival");

    server->QueueConsoleCommand("give spikes 1 Alice");
    Check(both(20.0, [&] { return a->client.GetResources(BlockType::SPIKES) == 64; }), "given spikes");
    const glm::ivec3 feet(glm::floor(a->player.GetFeetPosition()));
    a->client.PlaceBlock(feet, BlockType::SPIKES);
    Check(both(20.0, [&] { return a->client.GetHealth() < 20; }), "spikes hurt in survival");
    mine(feet);
    Check(both(20.0, [&] { return At(*b, feet).IsAir() && a->client.GetResources(BlockType::SPIKES) == 64; }), "and are worth a whole block when broken");

    std::cout << "Creative" << std::endl;
    a->client.SendChat("/gamemode creative");
    Check(both(20.0, [&] { return Has(*a, "Only operators can do that"); }) && !a->client.IsCreative(), "players can't change their own mode");
    server->QueueConsoleCommand("gamemode creative Alice");
    Check(both(20.0, [&] { return a->client.IsCreative() && Has(*a, "Server put you in creative mode"); }), "the console can");
    Check(both(20.0, [&] { return a->client.GetHealth() == 20; }), "and creative heals");
    a->client.SendChat("/gamemode");
    Check(both(20.0, [&] { return Has(*a, "You're in creative mode"); }), "/gamemode says which mode you're in");
    const uint32_t stoneBefore = a->client.GetResources(BlockType::STONE);
    for (int x = 0; x < 5; x++) a->client.PlaceBlock({ 20 + x, 0, 4 }, BlockType::STONE);
    a->client.PlaceBlock({ 26, 0, 4 }, BlockType::WATER);
    Check(both(20.0, [&] { return At(*b, { 24, 0, 4 }).type == BlockType::STONE && At(*b, { 26, 0, 4 }).HasWater(); }) &&
              a->client.GetResources(BlockType::STONE) == stoneBefore,
          "blocks (and water) cost nothing in creative");
    const uint32_t bricks = a->client.GetResources(BlockType::STONE_BRICK);
    a->client.BreakBlock(brick);
    Check(both(20.0, [&] { return At(*b, brick).IsAir(); }) && a->client.GetResources(BlockType::STONE_BRICK) == bricks,
          "and breaking them gives nothing (survival resources are kept as they were)");
    const glm::ivec3 creativeFeet(glm::floor(a->player.GetFeetPosition()));
    a->client.PlaceBlock(creativeFeet, BlockType::SPIKES);
    both(2.0, [] { return false; });
    Check(At(*b, creativeFeet).type == BlockType::SPIKES && a->client.GetHealth() == 20, "nothing hurts in creative");
    a->client.BreakBlock(creativeFeet);
    server->QueueConsoleCommand("op Alice");
    both(20.0, [&] { return Has(*a, "You are now an operator"); });
    a->client.SendChat("/gamemode survival Bob");
    Check(both(20.0, [&] { return Has(*b, "Alice put you in survival mode"); }), "operators can set others' modes");
    a->client.SendChat("/gamemode survival");
    Check(both(20.0, [&] { return !a->client.IsCreative(); }), "and their own");

    std::cout << "Saving" << std::endl;
    a->client.Disconnect();
    b->client.Disconnect();
    a.reset();
    b.reset();
    server->Stop();
    server.reset();
    // A world keeps the default it was made with, whatever the server's told now.
    server = std::make_unique<Server>();
    if (!server->Start(ServerOptions(dir, net::GameMode::Creative))) return EXIT_FAILURE;
    server->RunInBackground();
    a = std::make_unique<TestClient>();
    auto c = std::make_unique<TestClient>();
    a->client.Connect("127.0.0.1", server->GetPort(), "Alice", error, kTestViewDistance);
    c->client.Connect("127.0.0.1", server->GetPort(), "Carol", error, kTestViewDistance);
    Check(Pump({ a.get(), c.get() }, 30.0, [&] { return a->player.IsSpawned() && c->player.IsSpawned() && a->client.GetResources(BlockType::STONE_BRICK) > 0; }),
          "Alice comes back after a restart");
    Check(!a->client.IsCreative() && a->client.GetResources(BlockType::STONE_BRICK) == 32 && a->client.GetResources(BlockType::SPIKES) == 64 &&
              a->client.GetResources(groundType) == 32,
          "still in survival, with what she had (bricks " + std::to_string(a->client.GetResources(BlockType::STONE_BRICK)) + ", spikes " +
              std::to_string(a->client.GetResources(BlockType::SPIKES)) + ", ground " + std::to_string(a->client.GetResources(groundType)) + ")");
    Check(!c->client.IsCreative(), "a newcomer starts in the world's default mode (survival), not the server's new one");
    a->client.Disconnect();
    c->client.Disconnect();
    Pump({ a.get(), c.get() }, 0.3, [] { return false; });
    server->Stop();
    fs::remove_all(dir);

    std::cout << (failures ? "MODE TEST FAILED (" : "ALL MODE CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
