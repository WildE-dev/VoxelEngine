// Playing on the local network: a server opened to LAN and announced, a browser that
// finds it, and two clients that join it through the LAN listener, all in this process
// (on the loopback interface only, and an unusual discovery port, so neither a firewall
// nor a real game nearby is involved). Checks that
//  - the game is heard announced, with its host's and world's names,
//  - both players join at the address and port announced, and see each other's edits,
//  - the announced player count follows them,
//  - closing it takes it off the list at once,
//  - a dedicated server announced (without forwarding) is listed at its own port.
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "app/Player.h"
#include "client/Client.h"
#include "client/ClientWorld.h"
#include "net/Lan.h"
#include "server/Server.h"

constexpr int kTestViewDistance = 3;

namespace {

using Clock = std::chrono::steady_clock;

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

bool Wait(double seconds, const std::function<bool()>& done, const std::function<void(float)>& step = nullptr) {
    const auto deadline = Clock::now() + std::chrono::duration<double>(seconds);
    auto last = Clock::now();
    while (Clock::now() < deadline) {
        const auto now = Clock::now();
        if (step) step(std::chrono::duration<float>(now - last).count());
        last = now;
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

bool BlockIs(const ClientWorld& world, const glm::ivec3& p, BlockType type) {
    Block block;
    return world.GetBlock(p.x, p.y, p.z, block) && block.type == type;
}

} // namespace

int main() {
    // (Not the real discovery port: a game on this machine would hear the test.)
    const uint16_t discovery = static_cast<uint16_t>(28400 + std::chrono::steady_clock::now().time_since_epoch().count() % 400);

    Server server;
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.worldType = TerrainGenerator::Type::Empty;
    options.defaultMode = net::GameMode::Creative;
    options.log = false;
    if (!server.Start(options)) return EXIT_FAILURE;
    server.RunInBackground();

    std::string addresses;
    for (const std::string& address : net::LocalAddresses()) addresses += " " + address;
    std::cout << "        (this machine on its networks:" << (addresses.empty() ? " none" : addresses) << ")" << std::endl;

    net::LanBrowser browser;
    browser.loopbackOnly = true;
    std::string error;
    Check(browser.Start(error, discovery), "the browser listens for games" + (error.empty() ? "" : " (" + error + ")"));

    net::LanHost host;
    host.loopbackOnly = true;
    Check(host.Start("Hostess", "Test World", server.GetPort(), true, error, discovery), "the game opens to LAN" + (error.empty() ? "" : " (" + error + ")"));
    Check(host.GetPort() != 0 && host.GetPort() != server.GetPort(), "listening at a port of its own (" + std::to_string(host.GetPort()) + ")");

    net::LanGame found;
    std::string lookingFor = "Hostess";
    auto heard = [&] {
        for (const net::LanGame& g : browser.GetGames())
            if (g.name == lookingFor) {
                found = g;
                return true;
            }
        return false;
    };
    Check(Wait(5.0, heard), "the browser hears it announced");
    Check(found.world == "Test World" && found.port == host.GetPort() && found.compatible && found.players == 1,
          "as Hostess's Test World, at its port, the same version, one playing (" + found.address + ":" + std::to_string(found.port) + ")");

    TestClient a, b;
    Check(a.client.Connect(found.address, found.port, "Alice", error, kTestViewDistance), "Alice joins at the announced address" + (error.empty() ? "" : " (" + error + ")"));
    Check(b.client.Connect(found.address, found.port, "Bob", error, kTestViewDistance), "and Bob" + (error.empty() ? "" : " (" + error + ")"));
    if (failures) return EXIT_FAILURE;
    auto step = [&](float dt) {
        a.Update(dt);
        b.Update(dt);
    };
    Check(Wait(60.0, [&] { return a.player.IsSpawned() && b.player.IsSpawned(); }, step), "both land on terrain streamed over the LAN listener");
    const glm::ivec3 spot = glm::ivec3(glm::floor(a.player.GetFeetPosition())) + glm::ivec3(2, 0, 1);
    a.client.PlaceBlock(spot, BlockType::STONE);
    Check(Wait(20.0, [&] { return BlockIs(b.world, spot, BlockType::STONE); }, step), "stone Alice places appears for Bob");
    Check(Wait(5.0, [&] { return heard() && found.players == 3; }, step), "the announcement counts them: 3 playing");

    a.client.Disconnect();
    b.client.Disconnect();
    host.Stop();
    Check(Wait(1.0, [&] { return !heard(); }), "closing it takes it off the list at once");

    lookingFor = "Server";
    net::LanHost dedicated;
    dedicated.loopbackOnly = true;
    Check(dedicated.Start("Server", "world", server.GetPort(), false, error, discovery), "a dedicated server is announced");
    Check(Wait(5.0, heard) && found.port == server.GetPort() && found.players == -1, "and listed at its own port, players not said");
    dedicated.Stop();

    browser.Stop();
    server.Stop();
    std::cout << (failures ? "SOME LAN CHECKS FAILED (" : "ALL LAN CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
