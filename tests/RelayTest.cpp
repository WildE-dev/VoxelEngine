// Playing through the relay (VoxelRelay): starts a relay, a server opened to friends
// through it, and two clients that join with the code, all in this process over real
// UDP sockets on localhost, and checks that
//  - the host gets a join code, and a wrong code is refused,
//  - both friends join by code, land on streamed terrain, and see each other's edits,
//  - closing the game closes its room.
#include <atomic>
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
#include "net/RelayHost.h"
#include "relay/RelayProtocol.h"
#include "relay/RelayServer.h"
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
    // Join codes, as typed.
    Check(relay::LooksLikeCode("amber-otter-42") && relay::LooksLikeCode(" Amber Otter 42 ") && !relay::LooksLikeCode("localhost") &&
              !relay::LooksLikeCode("example.com") && !relay::LooksLikeCode("10.0.0.2:25565") && !relay::LooksLikeCode("my-server-host"),
          "join codes are told apart from addresses");

    relay::RelayServer relayServer;
    relay::RelayConfig config;
    config.bindHost = "127.0.0.1";
    config.port = 0;
    config.log = false;
    std::string error;
    Check(relayServer.Start(config, error), "the relay starts" + (error.empty() ? "" : " (" + error + ")"));
    if (failures) return EXIT_FAILURE;
    std::atomic<bool> stop{ false };
    std::atomic<int> rooms{ 0 };
    std::thread relayThread([&] {
        while (!stop) {
            relayServer.RunOnce(10);
            rooms = relayServer.RoomCount();
        }
    });
    const std::string relayAddress = "127.0.0.1:" + std::to_string(relayServer.GetPort());

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

    net::RelayHost host;
    Check(host.Start(relayAddress, server.GetPort(), error), "the game opens to friends through the relay");
    const bool open = Wait(10.0, [&] { return host.GetState() == net::RelayHost::State::Open; });
    const std::string code = host.GetCode();
    Check(open, "and gets a join code: " + code);
    Check(relay::LooksLikeCode(code), "which looks like one");

    {
        TestClient stranger;
        error.clear();
        const bool joined = stranger.client.Connect("127.0.0.1", relayServer.GetPort(), "Stranger", error, kTestViewDistance, "wrong-code-11");
        Check(!joined && error.find("No game") != std::string::npos, "a wrong code is refused (" + error + ")");
    }

    TestClient a, b;
    error.clear();
    Check(a.client.Connect("127.0.0.1", relayServer.GetPort(), "Alice", error, kTestViewDistance, code), "Alice joins by code" + (error.empty() ? "" : " (" + error + ")"));
    // (Typed loosely: capitals and spaces.)
    std::string loose = code;
    for (char& c : loose) c = c == '-' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    Check(b.client.Connect("127.0.0.1", relayServer.GetPort(), "Bob", error, kTestViewDistance, loose), "Bob joins, typing it as \"" + loose + "\"");
    if (failures) {
        stop = true;
        relayThread.join();
        return EXIT_FAILURE;
    }
    auto step = [&](float dt) {
        a.Update(dt);
        b.Update(dt);
    };
    Check(Wait(60.0, [&] { return a.player.IsSpawned() && b.player.IsSpawned(); }, step), "both land on terrain streamed through the relay");
    Check(Wait(10.0, [&] { return host.FriendCount() == 2; }, step), "the host sees two friends (" + host.GetStatus() + ")");

    const glm::ivec3 feet = glm::ivec3(glm::floor(a.player.GetFeetPosition()));
    const glm::ivec3 spot = feet + glm::ivec3(2, 0, 1);
    a.client.PlaceBlock(spot, BlockType::STONE);
    Check(Wait(20.0, [&] { return BlockIs(b.world, spot, BlockType::STONE); }, step), "stone Alice places appears for Bob");
    b.client.BreakBlock(spot);
    Check(Wait(20.0, [&] { return BlockIs(a.world, spot, BlockType::AIR); }, step), "and Bob breaking it shows for Alice");
    Check(a.client.IsConnected() && b.client.IsConnected(), "both still connected");

    a.client.Disconnect();
    b.client.Disconnect();
    host.Stop();
    Check(Wait(5.0, [&] { return rooms == 0; }), "closing the game closes its room on the relay");

    stop = true;
    relayThread.join();
    server.Stop();
    std::cout << (failures ? "SOME RELAY CHECKS FAILED (" : "ALL RELAY CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
