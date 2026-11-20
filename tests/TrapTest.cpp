// Traps, health and abilities over real connections: a server and two clients in an
// empty world, with physics running on the clients. Checks that
//  - spikes hurt, death is announced and respawns the player at the spawn point
//    with full health,
//  - crumbling bricks give way under a player, and the collapse spreads,
//  - a pressure plate opens the sluice gates near it,
//  - no-clip is an operator's command, the server tells the client, and it keeps
//    traps from hurting,
//  - a player who isn't an operator can't use the laser; an operator can.
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "app/Player.h"
#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "world/Collision.h"

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
            // As the game does: move there and fall, unless that's inside blocks.
            player.Teleport(*c);
            if (!player.IsFlying() && Collision::BoxBlocked(world, player.GetBounds())) player.ResetSpawn();
        }
        if (!joined) return;
        const bool noclip = client.GetAbilities() & net::kAbilityNoclip;
        if (noclip != player.IsFlying()) player.SetFlying(world, noclip);
        if (!player.IsSpawned() && !player.IsFlying()) player.Spawn(world);
        player.Update(world, PlayerInput{}, dt);
        world.Update(player.GetEyePosition());
        if (player.IsSpawned() || player.IsFlying())
            client.SendPlayerState(player.GetFeetPosition(), player.GetVelocity(), 0.0f, 0.0f, player.IsFlying() ? net::kFlagFlying : 0);
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

BlockType TypeAt(TestClient& c, const glm::ivec3& p) {
    Block b;
    return c.world.GetBlock(p.x, p.y, p.z, b) ? b.type : BlockType::COUNT;
}

bool Has(const TestClient& c, const std::string& text) {
    for (const auto& line : c.client.GetChat())
        if (line.text.find(text) != std::string::npos) return true;
    return false;
}

} // namespace

int main() {
    Server server;
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.log = false;
    options.worldType = TerrainGenerator::Type::Empty; // flat ground: the top of row -1 is at 0
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Survival; // traps only hurt in survival
    if (!server.Start(options)) return EXIT_FAILURE;
    server.RunInBackground();

    TestClient a, b;
    std::string error;
    a.client.Connect("127.0.0.1", server.GetPort(), "Tester", error);
    b.client.Connect("127.0.0.1", server.GetPort(), "Watcher", error);
    auto both = [&](double s, const std::function<bool()>& done) { return Pump({ &a, &b }, s, done); };
    Check(both(30.0, [&] { return a.player.IsSpawned() && b.player.IsSpawned(); }), "both players land on the ground");
    Check(a.client.GetHealth() == 20 && a.client.GetMaxHealth() == 20, "and start with full health (10 hearts)");
    // The tester builds the traps in survival, so needs the blocks; water can't be
    // placed in survival, so the watcher (in creative) pours it.
    for (const char* give : { "give spikes 4 Tester", "give crumbling_brick 4 Tester", "give sluice 2 Tester", "give pressure_plate 1 Tester" })
        server.QueueConsoleCommand(give);
    server.QueueConsoleCommand("gamemode creative Watcher");
    Check(both(5.0, [&] { return a.client.GetResources(BlockType::SPIKES) == 4 * 64 && a.client.GetResources(BlockType::PRESSURE_PLATE) == 64 && b.client.IsCreative(); }),
          "the tester is given trap blocks, the watcher goes creative");

    std::cout << "Spikes" << std::endl;
    const glm::ivec3 feet(glm::floor(a.player.GetFeetPosition()));
    a.client.PlaceBlock(feet, BlockType::SPIKES);
    Check(both(5.0, [&] { return a.client.GetHealth() < 20; }) && a.client.GetLastHurt() > 0.0,
          "standing in spikes hurts (" + std::to_string(a.client.GetHealth()) + " left)");
    Check(both(10.0, [&] { return Has(a, "Tester was impaled on spikes") && Has(b, "Tester was impaled on spikes"); }), "until death, which everyone hears about");
    a.client.BreakBlock(feet); // before falling back onto them
    Check(both(10.0, [&] { return a.client.GetHealth() == 20 && glm::distance(glm::vec2(a.player.GetFeetPosition().x, a.player.GetFeetPosition().z), glm::vec2(7.5f)) < 1.0f; }),
          "then respawns at the spawn point with full health");
    // Respawning drops the player from high up: wait for the landing (building has a reach).
    Check(both(15.0, [&] { return a.player.GetFeetPosition().y < 0.5f && a.player.IsOnGround(); }), "and lands");

    std::cout << "Crumbling bricks" << std::endl;
    for (int x = 20; x <= 23; x++) a.client.PlaceBlock({ x, 4, 7 }, BlockType::CRUMBLING_BRICK);
    both(1.0, [] { return false; });
    Check(TypeAt(b, { 20, 4, 7 }) == BlockType::CRUMBLING_BRICK && TypeAt(b, { 23, 4, 7 }) == BlockType::CRUMBLING_BRICK, "a ledge of crumbling bricks is built");
    server.QueueConsoleCommand("tp Tester 20.5 5.2 7.5");
    Check(both(5.0, [&] { return TypeAt(b, { 20, 4, 7 }) == BlockType::AIR; }), "it gives way under the player");
    Check(both(5.0, [&] { return TypeAt(b, { 23, 4, 7 }) == BlockType::AIR; }), "and the collapse spreads along the ledge");
    Check(both(5.0, [&] { return a.player.GetFeetPosition().y < 1.0f; }), "dropping the player to the ground");

    std::cout << "Pressure plates" << std::endl;
    a.client.PlaceBlock({ 30, 3, 7 }, BlockType::SLUICE);
    b.client.PlaceBlock({ 30, 4, 7 }, BlockType::WATER);
    a.client.PlaceBlock({ 30, 0, 10 }, BlockType::PRESSURE_PLATE);
    a.client.PlaceBlock({ 50, 3, 7 }, BlockType::SLUICE); // too far from the plate
    both(1.0, [] { return false; });
    Check(TypeAt(b, { 30, 3, 7 }) == BlockType::SLUICE && TypeAt(b, { 30, 0, 10 }) == BlockType::PRESSURE_PLATE, "a sluice gate holds water up; a plate is set nearby");
    server.QueueConsoleCommand("tp Tester 30.5 1 10.5");
    Check(both(5.0, [&] { return TypeAt(b, { 30, 3, 7 }) == BlockType::AIR && Has(a, "grinds open"); }), "stepping on the plate opens the gate");
    Check(TypeAt(b, { 50, 3, 7 }) == BlockType::SLUICE, "but not one out of its reach");
    Check(both(5.0, [&] { Block w; return b.world.GetBlock(30, 4, 7, w) && !w.HasWater(); }), "and the water it held pours out");

    std::cout << "No-clip and the laser" << std::endl;
    a.client.SendChat("/noclip");
    Check(both(5.0, [&] { return Has(a, "Only operators can do that"); }) && !(a.client.GetAbilities() & net::kAbilityNoclip), "a player can't turn on no-clip");
    const glm::ivec3 target(40, -1, 7);
    a.client.Carve({ target }, 1);
    both(1.0, [] { return false; });
    Check(TypeAt(b, target) != BlockType::AIR, "or use the laser: the server ignores it");
    server.QueueConsoleCommand("op Tester");
    Check(both(5.0, [&] { return a.client.GetAbilities() & net::kAbilityOperator; }), "once made an operator, the client is told");
    a.client.Carve({ target }, 1);
    Check(both(5.0, [&] { return TypeAt(b, target) == BlockType::AIR; }), "and the laser works");
    a.client.SendChat("/noclip");
    Check(both(5.0, [&] { return (a.client.GetAbilities() & net::kAbilityNoclip) && a.player.IsFlying(); }), "/noclip turns it on, and the client flies");
    const glm::ivec3 here(glm::floor(a.player.GetFeetPosition()));
    a.client.PlaceBlock(here, BlockType::SPIKES);
    both(2.0, [] { return false; });
    Check(a.client.GetHealth() == 20, "spikes can't hurt a player in no-clip");
    server.QueueConsoleCommand("deop Tester");
    Check(both(5.0, [&] { return !(a.client.GetAbilities() & (net::kAbilityNoclip | net::kAbilityOperator)) && !a.player.IsFlying(); }),
          "losing operator status ends no-clip too");

    a.client.Disconnect();
    b.client.Disconnect();
    server.Stop();
    std::cout << (failures ? "TRAP TEST FAILED (" : "ALL TRAP CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
