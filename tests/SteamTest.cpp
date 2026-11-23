// Lava (world/Lava.h) and steam (server/Steam.h). Checks that
//  - steam rises and spreads out from where it's let off, thins as it goes, and fades
//    away; it keeps out of solid blocks and water, and a block put where it is pushes
//    it up;
//  - lava runs kReach blocks out over the level from a source, pours down over an edge,
//    and drains away once its source is gone;
//  - water touching lava boils away into a big cloud of steam, and the lava stays;
//  - over a real connection: a creative player pours water beside lava they've put
//    down, the server boils it away, and the steam reaches the client; a survival
//    player can't put lava down.
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <tuple>

#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "server/Steam.h"
#include "world/Lava.h"
#include "world/Signals.h"
#include "world/TerrainGenerator.h"

constexpr int kTestViewDistance = 4;

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

// A stone floor under y = 0, air above, and whatever's set.
struct Cells : BlockAccess {
    std::map<std::tuple<int, int, int>, Block> blocks;
    bool GetBlock(int x, int y, int z, Block& block) const override {
        auto it = blocks.find({ x, y, z });
        block = it != blocks.end() ? it->second : (y < 0 ? Block(BlockType::STONE) : Block());
        return true;
    }
    Block At(int x, int y, int z) const {
        Block b;
        GetBlock(x, y, z, b);
        return b;
    }
    void Apply(const std::vector<Lava::Change>& changes) {
        for (const auto& c : changes) blocks[{ c.pos.x, c.pos.y, c.pos.z }] = c.block;
    }
    // Steps the lava cells (all of them, each time) until nothing changes.
    void RunLava(std::vector<Lava::Boil>* boils = nullptr, int steps = 40) {
        for (int i = 0; i < steps; i++) {
            std::vector<glm::ivec3> lava;
            for (const auto& [k, b] : blocks)
                if (b.IsLava()) lava.emplace_back(std::get<0>(k), std::get<1>(k), std::get<2>(k));
            std::vector<Lava::Change> changes;
            std::vector<Lava::Boil> made;
            Lava::Step(*this, lava, changes, made);
            if (boils) boils->insert(boils->end(), made.begin(), made.end());
            if (changes.empty()) return;
            Apply(changes);
        }
    }
};

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

} // namespace

int main() {
    std::cout << "Steam" << std::endl;
    {
        Cells world;
        SteamCloud steam;
        steam.Add({ 0, 2, 0 }, Block::kMaxWaterVolume * Lava::kSteamPerUnit); // (a block of water's worth)
        const long long start = steam.Total();
        for (int i = 0; i < 6; i++) steam.Update(world);
        int highest = 0, widest = 0;
        for (const auto& [cell, units] : steam.All()) highest = std::max(highest, cell.y), widest = std::max(widest, std::abs(cell.x));
        Check(steam.All().size() > 20 && highest >= 5 && widest >= 2 && steam.At({ 0, 2, 0 }) < 400,
              "a burst rises and spreads out (" + std::to_string(steam.All().size()) + " cells, up to y " + std::to_string(highest) + ")");
        Check(steam.Total() < start && steam.Total() > start / 2, "and fades as it goes (" + std::to_string(steam.Total()) + " of " + std::to_string(start) + ")");
        bool clean = true;
        for (const auto& [cell, units] : steam.All()) clean &= cell.y >= 0;
        Check(clean, "it keeps out of the ground");
        for (int i = 0; i < 240; i++) steam.Update(world); // (a minute)
        Check(steam.All().empty(), "and is gone in under a minute");

        // Under a roof it spreads sideways; a block put where it is pushes it up.
        Cells roofed;
        for (int x = -6; x <= 6; x++)
            for (int z = -6; z <= 6; z++) roofed.blocks[{ x, 3, z }] = Block(BlockType::STONE);
        SteamCloud under;
        under.Add({ 0, 1, 0 }, 1500);
        for (int i = 0; i < 20; i++) under.Update(roofed);
        bool belowRoof = true;
        for (const auto& [cell, units] : under.All()) belowRoof &= cell.y < 3;
        Check(belowRoof && under.At({ 2, 2, 0 }) > 0, "under a roof it spreads out beneath it");
        SteamCloud pushed;
        pushed.Add({ 0, 1, 0 }, 100);
        Cells filled;
        filled.blocks[{ 0, 1, 0 }] = Block(BlockType::STONE);
        pushed.Update(filled);
        Check(pushed.At({ 0, 1, 0 }) == 0 && pushed.At({ 0, 2, 0 }) > 0, "and a block put where it is pushes it up");
        Cells wet;
        wet.blocks[{ 1, 0, 0 }] = Block::Water(Block::kMaxWaterVolume);
        SteamCloud dry;
        dry.Add({ 0, 0, 0 }, 1000);
        for (int i = 0; i < 5; i++) dry.Update(wet);
        Check(dry.At({ 1, 0, 0 }) == 0, "it doesn't go into water");
    }

    std::cout << "Lava" << std::endl;
    {
        Cells world;
        world.blocks[{ 0, 0, 0 }] = Lava::Make(0, false);
        world.RunLava();
        Check(world.At(3, 0, 0).IsLava() && Lava::Level(world.At(3, 0, 0)) == 3 && !world.At(4, 0, 0).IsLava() && world.At(-2, 0, 1).IsLava(),
              "a source runs three blocks out over the level, thinner each block");
        world.blocks[{ 0, 0, 0 }] = Block();
        world.RunLava();
        bool gone = true;
        for (const auto& [k, b] : world.blocks) gone &= !b.IsLava();
        Check(gone, "and once its source is gone, it drains away");

        // Over an edge: a ledge at y 3 with open air beside it.
        Cells ledge;
        for (int x = -3; x <= 0; x++)
            for (int y = 0; y <= 2; y++) ledge.blocks[{ x, y, 0 }] = Block(BlockType::STONE);
        ledge.blocks[{ 0, 3, 0 }] = Lava::Make(0, false);
        ledge.RunLava();
        Check(ledge.At(1, 3, 0).IsLava() && ledge.At(1, 2, 0).IsLava() && Lava::Falling(ledge.At(1, 1, 0)) && ledge.At(1, 0, 0).IsLava() &&
                  ledge.At(3, 0, 0).IsLava(),
              "it pours down over an edge and runs on from where it lands");

        // Water beside it boils away into steam; the lava stays.
        Cells pool;
        pool.blocks[{ 0, 0, 0 }] = Lava::Make(0, false);
        pool.blocks[{ 0, 0, 1 }] = Block(BlockType::STONE);
        pool.blocks[{ 1, 0, 0 }] = Block::Water(Block::kMaxWaterVolume);
        std::vector<Lava::Boil> boils;
        pool.RunLava(&boils, 1);
        int made = 0;
        for (const auto& b : boils) made += b.steam;
        Check(!pool.At(1, 0, 0).HasWater() && pool.At(0, 0, 0).IsLava() && made == Block::kMaxWaterVolume * Lava::kSteamPerUnit,
              "water touching it boils away into " + std::to_string(made) + " units of steam, and the lava stays");
    }

    std::cout << "Over the network" << std::endl;
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.worldType = TerrainGenerator::Type::Empty; // grass at row -1
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Creative;
    options.creatures = false;
    Server server;
    if (!server.Start(options)) return EXIT_FAILURE;
    server.RunInBackground();
    TestClient a;
    std::string error;
    a.client.Connect("127.0.0.1", server.GetPort(), "Smith", error, kTestViewDistance);
    auto at = [&](const glm::ivec3& p) {
        Block b;
        return a.world.GetBlock(p.x, p.y, p.z, b) ? b : Block(BlockType::COUNT);
    };
    Check(Pump(a, 20.0, [&] { return a.joined && at({ 10, -1, 10 }).type == BlockType::GRASS; }), "a creative player joins");
    server.QueueConsoleCommand("tp Smith 7.5 0 7.5");
    Pump(a, 5.0, [&] { return std::abs(a.feet.x - 7.5f) < 0.1f; });
    // A pit of stone: lava in one end, and a wall between it and where the water goes.
    server.QueueConsoleCommand("fill 10 -3 10 14 -1 12 stone");
    Pump(a, 10.0, [&] { return at({ 14, -1, 12 }).type == BlockType::STONE; });
    a.client.BreakBlock({ 11, -1, 11 });
    a.client.BreakBlock({ 12, -1, 11 });
    Check(Pump(a, 10.0, [&] { return at({ 11, -1, 11 }).IsAir() && at({ 12, -1, 11 }).IsAir(); }), "the player digs a little trench");
    Check(a.client.PlaceBlock({ 11, -1, 11 }, BlockType::LAVA) && Pump(a, 10.0, [&] { return at({ 11, -1, 11 }).IsLava(); }),
          "puts lava in one end of it");
    Check(Pump(a, 10.0, [&] { return at({ 12, -1, 11 }).IsLava(); }), "and it runs along the trench");
    server.QueueConsoleCommand("fill 13 -1 11 13 -1 11 water");
    Check(Pump(a, 10.0, [&] { return !a.client.GetSteam().empty(); }), "water poured beside the lava boils away, and the steam reaches the client");
    Check(Pump(a, 5.0, [&] { return !at({ 13, -1, 11 }).HasWater() && at({ 12, -1, 11 }).IsLava(); }), "the water's gone, and the lava's still there");
    size_t biggest = 0;
    const auto boiled = std::chrono::steady_clock::now();
    const bool faded = Pump(a, 90.0, [&] {
        biggest = std::max(biggest, a.client.GetSteam().size());
        return a.client.GetSteam().empty();
    });
    const double lasted = std::chrono::duration<double>(std::chrono::steady_clock::now() - boiled).count();
    Check(faded && biggest > 60 && lasted > 5.0, "a big cloud (" + std::to_string(biggest) + " cells at most), faded away after " + std::to_string(static_cast<int>(lasted)) + " s");

    // Steam in pipes lets itself out: out of an open end's end face, and out of an outlet.
    auto steamAt = [&](const glm::ivec3& cell) {
        for (const auto& s : a.client.GetSteam())
            if (s.cell == cell) return true;
        return false;
    };
    server.QueueConsoleCommand("fill 16 0 14 18 0 14 fluid_pipe");
    server.QueueConsoleCommand("fill 18 0 13 18 0 13 stone"); // (beside an end: not where it comes out)
    Pump(a, 10.0, [&] { return at({ 18, 0, 14 }).type == BlockType::FLUID_PIPE; });
    server.QueueConsoleCommand("fluid 17 0 14 steam");
    bool left = false, right = false;
    Pump(a, 10.0, [&] {
        for (const auto& s : a.client.GetSteam()) {
            if (s.cell.x <= 15) left = true;
            if (s.cell.x >= 19) right = true;
        }
        return left && right;
    });
    std::string seen;
    for (const auto& s : a.client.GetSteam()) seen += " (" + std::to_string(s.cell.x) + "," + std::to_string(s.cell.y) + "," + std::to_string(s.cell.z) + ")";
    Check(left && right, "steam in a pipe lets itself out of both open ends" + (left && right ? std::string() : seen));
    Check(Pump(a, 20.0, [&] { return a.client.GetSteam().empty(); }), "until it's all out (and the cloud fades)");
    // Ends closed (stone in front of each), but an outlet on the pipe facing up.
    server.QueueConsoleCommand("fill 15 0 14 15 0 14 stone");
    server.QueueConsoleCommand("fill 19 0 14 19 0 14 stone");
    server.QueueConsoleCommand("fill 17 0 15 17 0 15 fluid_outlet");
    Pump(a, 10.0, [&] { return at({ 17, 0, 15 }).type == BlockType::FLUID_OUTLET; });
    server.QueueConsoleCommand("fluid 17 0 14 steam");
    Check(Pump(a, 10.0, [&] { return steamAt({ 18, 0, 15 }); }) && !steamAt({ 19, 1, 14 }) && !steamAt({ 15, 1, 14 }) ,
          "with its ends closed, it comes out of an outlet on the pipe (out of its front)");
    Pump(a, 20.0, [&] { return a.client.GetSteam().empty(); });
    // A shut fluid valve keeps the two sides apart: steam on one side doesn't reach the
    // other side's open end; opened, it does.
    server.QueueConsoleCommand("fill 16 0 20 20 0 20 fluid_pipe");
    server.QueueConsoleCommand("fill 18 0 20 18 0 20 fluid_valve");
    server.QueueConsoleCommand("fill 15 0 20 15 0 20 stone");
    Pump(a, 10.0, [&] { return at({ 18, 0, 20 }).type == BlockType::FLUID_VALVE && at({ 20, 0, 20 }).type == BlockType::FLUID_PIPE; });
    server.QueueConsoleCommand("fluid 16 0 20 steam");
    Check(!Pump(a, 4.0, [&] { return steamAt({ 21, 0, 20 }); }), "a shut fluid valve keeps steam from the far side");
    server.QueueConsoleCommand("tp Smith 18.5 0 17.5"); // (within reach of it)
    Pump(a, 5.0, [&] { return std::abs(a.feet.z - 17.5f) < 0.1f; });
    a.client.UseBed({ 18, 0, 20 });
    Check(Pump(a, 5.0, [&] { return at({ 18, 0, 20 }).type == BlockType::FLUID_VALVE_OPEN; }), "a fluid valve opens by hand");
    server.QueueConsoleCommand("fluid 16 0 20 steam");
    Check(Pump(a, 10.0, [&] { return steamAt({ 21, 0, 20 }); }), "and open, the steam gets through to the far end");

    server.QueueConsoleCommand("gamemode survival Smith");
    Pump(a, 10.0, [&] { return !a.client.IsCreative(); });
    Check(!a.client.PlaceBlock({ 10, 0, 10 }, BlockType::LAVA) && !a.client.PlaceBlock({ 10, 0, 10 }, BlockType::AIR),
          "a survival player can't put lava down (and an empty hand places nothing)");
    // Standing in the trench beside the lava when water comes in: the steam scalds.
    server.QueueConsoleCommand("tp Smith 13.5 -1 11.5");
    Pump(a, 5.0, [&] { return std::abs(a.feet.y + 1.0f) < 0.1f; });
    Pump(a, 1.5, [] { return false; }); // (past being just teleported)
    const int healthBefore = a.client.GetHealth();
    server.QueueConsoleCommand("fill 13 -1 11 13 -1 11 water");
    const bool scalded = Pump(a, 10.0, [&] { return a.client.GetHealth() < healthBefore; });
    Check(scalded, "thick steam scalds a survival player in it (" + std::to_string(healthBefore) + " -> " + std::to_string(a.client.GetHealth()) + " half hearts)");

    a.client.Disconnect();
    Pump(a, 0.3, [] { return false; });
    server.Stop();
    std::cout << (failures ? "STEAM TEST FAILED (" : "ALL STEAM CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
