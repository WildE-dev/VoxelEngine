// Sound (no speakers needed). Checks that
//  - every sound the game plays exists in Resources/Sounds and loads (16-bit mono),
//    footsteps have a few variants, and the rain loop is long enough to loop,
//  - blocks sound like what they're made of,
//  - over a real connection, a player hears what another player builds and breaks
//    (block events), and not their own edits a second time.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "audio/Audio.h"
#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"

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
    std::vector<Client::BlockEvent> events;
    void Update() {
        client.Poll();
        for (const auto& e : client.TakeBlockEvents()) events.push_back(e);
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

} // namespace

int main() {
    std::cout << "Sound files" << std::endl;
    const std::string dir = "Resources/Sounds";
    // Everything Application plays, by name (variants: name_N).
    const char* names[] = { "step_soft", "step_hard", "step_sand", "step_wood", "dig_soft", "dig_hard", "break_soft", "break_hard",
                            "break_wood", "break_glass", "place", "pickup", "craft", "hurt", "hit", "splash", "groan", "rain_loop", "eat", "thunder", "bow", "birds", "crickets" };
    bool all = true;
    std::string missing;
    for (const char* name : names) {
        Audio::Clip clip;
        const bool plain = Audio::LoadWav(dir + "/" + name + ".wav", clip);
        const bool variant = Audio::LoadWav(dir + "/" + name + "_1.wav", clip);
        if (!plain && !variant) {
            all = false;
            missing += std::string(" ") + name;
        }
    }
    Check(all, "every sound the game plays is there and loads" + (missing.empty() ? std::string() : " (missing:" + missing + ")"));
    int stepVariants = 0;
    for (int v = 1; v <= 9; v++) stepVariants += fs::exists(dir + "/step_soft_" + std::to_string(v) + ".wav");
    Check(stepVariants >= 3, "footsteps have a few variants (" + std::to_string(stepVariants) + "), so walking doesn't sound like a metronome");
    Audio::Clip rain;
    Check(Audio::LoadWav(dir + "/rain_loop.wav", rain) && rain.samples.size() >= static_cast<size_t>(rain.rate), "the rain loop is a second or more");
    Audio::Clip bad;
    Check(!Audio::LoadWav(dir + "/no_such_sound.wav", bad) && !Audio::LoadWav("CMakeCache.txt", bad), "missing or non-WAV files are refused");

    std::cout << "Materials" << std::endl;
    Check(std::string(Audio::Material(BlockType::STONE)) == "hard" && std::string(Audio::Material(BlockType::GRASS)) == "soft" &&
              std::string(Audio::Material(BlockType::SAND)) == "sand" && std::string(Audio::Material(BlockType::PLANKS)) == "wood" &&
              std::string(Audio::Material(BlockType::GLASS)) == "glass",
          "stone sounds hard, grass soft, sand sandy, planks wooden, glass like glass");

    std::cout << "Others' edits" << std::endl;
    Server server;
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = false;
    options.worldType = TerrainGenerator::Type::Empty;
    options.defaultMode = net::GameMode::Creative;
    if (!server.Start(options)) return EXIT_FAILURE;
    server.RunInBackground();
    TestClient a, b;
    std::string error;
    a.client.Connect("127.0.0.1", server.GetPort(), "Builder", error, kTestViewDistance);
    b.client.Connect("127.0.0.1", server.GetPort(), "Listener", error, kTestViewDistance);
    auto both = [&](double s, const std::function<bool()>& done) { return Pump({ &a, &b }, s, done); };
    auto groundIn = [](TestClient& c) { Block g; return c.world.GetBlock(10, -1, 10, g) && !g.IsAir(); };
    Check(both(20.0, [&] { return a.joined && b.joined && groundIn(a) && groundIn(b); }), "two players join");
    a.events.clear();
    b.events.clear();
    a.client.PlaceBlock({ 10, 0, 10 }, BlockType::PLANKS);
    auto has = [](const TestClient& c, bool placed, BlockType type) {
        for (const auto& e : c.events)
            if (e.placed == placed && e.type == type && e.pos == glm::ivec3(10, 0, 10)) return true;
        return false;
    };
    Check(both(20.0, [&] { return has(b, true, BlockType::PLANKS); }), "the other player hears planks being placed");
    a.client.BreakBlock({ 10, 0, 10 });
    Check(both(20.0, [&] { return has(b, false, BlockType::PLANKS); }), "and broken");
    both(1.0, [] { return false; });
    Check(!has(a, true, BlockType::PLANKS) && !has(a, false, BlockType::PLANKS), "the builder doesn't get their own edits back as events");
    a.client.Disconnect();
    b.client.Disconnect();
    Pump({ &a, &b }, 0.3, [] { return false; });
    server.Stop();

    std::cout << (failures ? "SOUND TEST FAILED (" : "ALL SOUND CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
