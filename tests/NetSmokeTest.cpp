// End-to-end check of the client/server split, without windows or GL:
// starts a server and two clients in this process (over real ENet sockets on
// localhost), lets both players land on the terrain, then checks that
//  - both receive the chunks around them,
//  - a block placed / broken by one client shows up on the other,
//  - water poured by one client flows, and both clients end up seeing the same water,
//  - each client sees the other player where it actually is,
//  - a flood of ~100 water blocks streams to the clients at a modest rate.
//
// Usage: NetSmokeTest [host:port]   (with an address, joins that server instead of
// starting one; run two at once against a dedicated server for a 4-client test).
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "app/Player.h"
#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"

namespace {

using Clock = std::chrono::steady_clock;

struct TestClient {
    std::string name;
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
        if (auto c = client.TakeCorrection()) player.Teleport(*c);
        if (!joined) return;
        if (!player.IsSpawned()) player.Spawn(world);
        player.Update(world, PlayerInput{}, dt);
        world.Update(player.GetEyePosition());
        if (player.IsSpawned()) client.SendPlayerState(player.GetFeetPosition(), player.GetVelocity(), 0.0f, 0.0f, 0);
    }
};

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

// Runs both clients until `done` returns true or the timeout passes.
bool Pump(TestClient& a, TestClient& b, double seconds, const std::function<bool()>& done) {
    const auto deadline = Clock::now() + std::chrono::duration<double>(seconds);
    auto last = Clock::now();
    while (Clock::now() < deadline) {
        const auto now = Clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        a.Update(dt);
        b.Update(dt);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

bool BlockIs(const ClientWorld& world, const glm::ivec3& p, BlockType type) {
    Block block;
    return world.GetBlock(p.x, p.y, p.z, block) && block.type == type;
}

// The first air cell above the ground in a column, searching down from `top`.
bool FindGround(const ClientWorld& world, int x, int z, int top, glm::ivec3& above) {
    for (int y = top; y > top - 40; y--) {
        Block block;
        if (!world.GetBlock(x, y, z, block)) return false;
        if (block.IsPlant() || block.HasWater()) return false; // look elsewhere
        if (!block.IsPassable()) {                              // ground: a full block or a slope
            Block cell;
            if (!world.GetBlock(x, y + 1, z, cell) || !cell.IsAir()) return false;
            above = { x, y + 1, z };
            return true;
        }
    }
    return false;
}

struct WaterView {
    int cells = 0;
    int total = 0; // shown volume
};

WaterView CountWater(const ClientWorld& world, const glm::ivec3& center, int r) {
    WaterView view;
    for (int x = -r; x <= r; x++)
        for (int y = -3; y <= 1; y++)
            for (int z = -r; z <= r; z++) {
                Block block;
                if (world.GetBlock(center.x + x, center.y + y, center.z + z, block) && block.HasWater()) {
                    view.cells++;
                    view.total += block.shown;
                }
            }
    return view;
}

// Cells in the box where the two worlds disagree on whether there's water, or on its
// drawn level.
int WaterMismatches(const ClientWorld& a, const ClientWorld& b, const glm::ivec3& center, int r) {
    int mismatches = 0;
    for (int x = -r; x <= r; x++)
        for (int y = -3; y <= 1; y++)
            for (int z = -r; z <= r; z++) {
                Block ba, bb;
                const glm::ivec3 p = center + glm::ivec3(x, y, z);
                if (!a.GetBlock(p.x, p.y, p.z, ba) || !b.GetBlock(p.x, p.y, p.z, bb)) continue;
                if (ba.type != bb.type || ba.HasWater() != bb.HasWater() || ba.WaterSurface32nds() != bb.WaterSurface32nds()) mismatches++;
            }
    return mismatches;
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    uint16_t port = 0;
    std::unique_ptr<Server> server;
    if (argc > 1) {
        const std::string target = argv[1];
        const size_t colon = target.rfind(':');
        host = target.substr(0, colon);
        port = colon == std::string::npos ? net::kDefaultPort : static_cast<uint16_t>(std::atoi(target.c_str() + colon + 1));
    }
    else {
        server = std::make_unique<Server>();
        Server::Options options;
        options.bindHost = "127.0.0.1";
        options.port = 0;
        options.defaultMode = net::GameMode::Creative; // building freely
        if (!server->Start(options)) return EXIT_FAILURE;
        server->RunInBackground();
        port = server->GetPort();
    }

    TestClient a, b;
    a.name = "Alice";
    b.name = "Bob";
    std::string error;
    Check(a.client.Connect(host, port, a.name, error), "client A connects" + (error.empty() ? "" : " (" + error + ")"));
    Check(b.client.Connect(host, port, b.name, error), "client B connects" + (error.empty() ? "" : " (" + error + ")"));
    if (failures) return EXIT_FAILURE;

    const auto start = Clock::now();
    Check(Pump(a, b, 60.0, [&] { return a.player.IsSpawned() && b.player.IsSpawned(); }), "both players land on streamed terrain");
    std::cout << "        (took " << std::chrono::duration<double>(Clock::now() - start).count() << " s; A has "
              << a.world.GetLoadedChunkCount() << " chunks, B has " << b.world.GetLoadedChunkCount() << ")" << std::endl;
    if (failures) return EXIT_FAILURE;

    // Players see each other where they are.
    Pump(a, b, 1.0, [] { return false; });
    auto seesOther = [](const TestClient& viewer, const TestClient& other) {
        for (const auto& p : viewer.client.GetRemotePlayers())
            if (p.id == other.client.GetPlayerId()) return glm::distance(p.feet, other.player.GetFeetPosition()) < 0.5f;
        return false;
    };
    Check(seesOther(a, b), "A sees B at B's position");
    Check(seesOther(b, a), "B sees A at A's position");

    // Block edits from A reach B, and the other way round.
    const glm::ivec3 feetA = glm::ivec3(glm::floor(a.player.GetFeetPosition()));
    glm::ivec3 spot;
    bool found = false;
    for (int dx = 2; dx < 8 && !found; dx++) found = FindGround(a.world, feetA.x + dx, feetA.z + 1, feetA.y + 6, spot);
    Check(found, "found flat ground near A");
    if (found) {
        const glm::ivec3 pillar = spot + glm::ivec3(0, 1, 0);
        a.client.PlaceBlock(pillar, BlockType::STONE);
        Check(Pump(a, b, 5.0, [&] { return BlockIs(b.world, pillar, BlockType::STONE); }), "stone placed by A appears on B");
        b.client.BreakBlock(pillar);
        Check(Pump(a, b, 5.0, [&] { return BlockIs(a.world, pillar, BlockType::AIR); }), "stone broken by B disappears on A");
    }

    // Water poured by A flows, on both clients.
    glm::ivec3 pool;
    found = false;
    for (int dx = -6; dx < 8 && !found; dx++) found = FindGround(a.world, feetA.x + dx, feetA.z - 3, feetA.y + 6, pool);
    Check(found, "found ground for water near A");
    if (found) {
        // Several blocks of water, so it has enough volume to spread visibly.
        for (int i = 0; i < 4; i++) a.client.PlaceBlock(pool + glm::ivec3(0, i, 0), BlockType::WATER);
        Check(Pump(a, b, 5.0, [&] { return CountWater(b.world, pool, 6).cells >= 1; }), "water placed by A appears on B");
        const bool spread = Pump(a, b, 15.0, [&] { return CountWater(b.world, pool, 6).cells >= 6; });
        const WaterView va = CountWater(a.world, pool, 6), vb = CountWater(b.world, pool, 6);
        Check(spread, "water flows outward as seen by B (" + std::to_string(vb.cells) + " wet cells)");
        // Let it settle, then both views must match.
        Pump(a, b, 8.0, [] { return false; });
        const int mismatches = WaterMismatches(a.world, b.world, pool, 6);
        Check(mismatches == 0, "A and B see the same water (" + std::to_string(mismatches) + " differing cells, A " +
                                   std::to_string(va.cells) + " / B " + std::to_string(vb.cells) + " wet cells mid-flow)");
    }

    // A flood: a 6x6x3 block of water (108 full cells) dropped on the terrain spreads
    // over many ticks. Measure what that costs each client on the wire.
    {
        const glm::ivec3 feetB = glm::ivec3(glm::floor(b.player.GetFeetPosition()));
        const glm::ivec3 base(feetB.x - 3, feetB.y + 3, feetB.z + 6);
        const auto before = b.client.GetStats();
        int placed = 0;
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 6; x++)
                for (int z = 0; z < 6; z++) {
                    Block cell;
                    const glm::ivec3 p = base + glm::ivec3(x, y, z);
                    if (b.world.GetBlock(p.x, p.y, p.z, cell) && cell.IsAir()) {
                        b.client.PlaceBlock(p, BlockType::WATER);
                        placed++;
                    }
                }
        const double seconds = 10.0;
        Pump(a, b, seconds, [] { return false; });
        const auto after = b.client.GetStats();
        const double kbPerSecond = (after.bytesReceived - before.bytesReceived) / 1024.0 / seconds;
        // Water that has left the box it was placed in.
        int outside = 0;
        for (int x = -14; x <= 20; x++)
            for (int y = -24; y <= 3; y++)
                for (int z = -14; z <= 20; z++) {
                    const bool inBox = x >= 0 && x < 6 && y >= 0 && y < 3 && z >= 0 && z < 6;
                    Block cell;
                    const glm::ivec3 p = base + glm::ivec3(x, y, z);
                    if (!inBox && a.world.GetBlock(p.x, p.y, p.z, cell) && cell.HasWater()) outside++;
                }
        std::cout << "        flood: " << placed << " cells placed, " << outside << " wet cells outside that box after " << seconds << " s, "
                  << (after.deltasReceived - before.deltasReceived) << " cell updates, " << kbPerSecond << " KB/s to each client"
                  << std::endl;
        Check(placed > 50 && outside > 30, "flood spreads (seen by A)");
        Check(kbPerSecond < 256.0, "flood costs under 256 KB/s per client");
    }

    const Client::Stats sa = a.client.GetStats(), sb = b.client.GetStats();
    std::cout << "        A received " << sa.bytesReceived / 1024 << " KB (" << sa.chunksReceived << " chunks, " << sa.deltasReceived
              << " cell updates, " << sa.lightUpdates << " light updates)" << std::endl;
    std::cout << "        B received " << sb.bytesReceived / 1024 << " KB (" << sb.chunksReceived << " chunks, " << sb.deltasReceived
              << " cell updates, " << sb.lightUpdates << " light updates)" << std::endl;

    a.client.Disconnect();
    b.client.Disconnect();
    if (server) server->Stop();
    std::cout << (failures ? "FAILED" : "ALL PASSED") << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
