// Chat and commands over real connections: a server and two clients in this process.
// Checks that
//  - joining is announced, and a chat line reaches everyone with the sender's name,
//  - control characters are stripped and empty lines ignored,
//  - /msg reaches only its target, /me and /list work,
//  - /time and /weather change the sky for everyone,
//  - /tp moves a player (by coordinates, relative, or to another player),
//  - mistakes get an error back only to whoever made them,
//  - console commands run (with or without the slash),
//  - only operators can change things: the console makes one, operators make and
//    unmake others, the list survives a restart (ops.txt), and the owner (the
//    singleplayer host) always is one.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "app/PlayerNames.h"

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
};

bool Pump(std::initializer_list<TestClient*> clients, double seconds, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto* c : clients) c->client.Poll();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

// Whether a client's chat log has a line containing `text` (of `kind`, if given).
bool Has(const TestClient& c, const std::string& text, int kind = -1) {
    for (const auto& line : c.client.GetChat())
        if (line.text.find(text) != std::string::npos && (kind < 0 || static_cast<int>(line.kind) == kind)) return true;
    return false;
}

constexpr int kChat = 0, kInfo = 1, kError = 2, kPrivate = 3;

} // namespace

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

int main() {
    std::random_device rd;
    const auto dir = std::filesystem::temp_directory_path() / ("voxel-chat-test-" + std::to_string(rd()));
    Server server;
    Server::Options options;
    options.worldDirectory = dir.string();
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = false;
    options.worldType = TerrainGenerator::Type::Empty;
    options.dayLengthSeconds = 100000.0f; // the clock stays where commands put it
    options.weather = Server::Options::Weather::Clear;
    if (!server.Start(options)) return EXIT_FAILURE;
    server.RunInBackground();

    TestClient a, b;
    std::string error;
    Check(a.client.Connect("127.0.0.1", server.GetPort(), "Alice", error, kTestViewDistance), "Alice connects");
    Check(Pump({ &a }, 20.0, [&] { return a.client.IsWelcomed(); }), "and is welcomed");
    Check(Pump({ &a }, 20.0, [&] { return Has(a, "/help", kInfo); }), "with a hint about chat and commands");
    Check(b.client.Connect("127.0.0.1", server.GetPort(), "Bob", error, kTestViewDistance), "Bob connects");
    Check(Pump({ &a, &b }, 20.0, [&] { return Has(a, "Bob joined the game", kInfo); }), "Alice sees Bob join");
    auto both = [&](double s, const std::function<bool()>& done) { return Pump({ &a, &b }, s, done); };
    {
        // A second "alice" (any capitals) is turned away, told why; Alice plays on.
        TestClient twin;
        twin.client.Connect("127.0.0.1", server.GetPort(), "alice", error, kTestViewDistance);
        Pump({ &a, &b, &twin }, 20.0, [&] { return !twin.client.GetDisconnectReason().empty(); });
        const std::string why = twin.client.GetDisconnectReason();
        Check(!twin.client.IsWelcomed() && why.find("already playing here") != std::string::npos && a.client.IsConnected(),
              "a second player with a name in use is turned away (" + why + ")");
        // A player whose connection has gone quiet (dropped, not yet timed out) coming
        // back under the same name is let in, and the old connection let go.
        TestClient gone, back;
        gone.client.Connect("127.0.0.1", server.GetPort(), "Dave", error, kTestViewDistance);
        Pump({ &a, &b, &gone }, 20.0, [&] { return gone.client.IsWelcomed(); });
        Pump({ &a, &b }, 4.0, [] { return false; }); // (Dave's game says nothing now)
        back.client.Connect("127.0.0.1", server.GetPort(), "Dave", error, kTestViewDistance);
        Check(Pump({ &a, &b, &back }, 20.0, [&] { return back.client.IsWelcomed(); }) && back.client.GetDisconnectReason().empty(),
              "a player whose old connection went quiet can come straight back under their name");
        back.client.Disconnect();
        const std::string made = PlayerNames::Random(7), again = PlayerNames::Random(7), other = PlayerNames::Random(8);
        Check(!made.empty() && made.size() <= 32 && made == again && made != other, "a new player's name is made up (" + made + ", " + other + ")");
    }

    std::cout << "Chat" << std::endl;
    a.client.SendChat("hello there");
    Check(both(20.0, [&] { return Has(a, "<Alice> hello there", kChat) && Has(b, "<Alice> hello there", kChat); }),
          "a chat line reaches everyone, with the sender's name");
    a.client.SendChat(std::string("bell\x07 and\ttab"));
    Check(both(20.0, [&] { return Has(b, "<Alice> bell andtab"); }), "control characters are stripped");
    const size_t before = b.client.GetChat().size();
    a.client.SendChat("    ");
    both(0.5, [] { return false; });
    Check(b.client.GetChat().size() == before, "blank lines are ignored");

    std::cout << "Permissions" << std::endl;
    b.client.SendChat("/time noon");
    Check(both(20.0, [&] { return Has(b, "Only operators can do that", kError); }), "a player who isn't an operator can't set the time");
    b.client.SendChat("/time");
    Check(both(20.0, [&] { return Has(b, "It's ", kInfo); }), "but can see it");
    server.QueueConsoleCommand("op Alice");
    Check(both(20.0, [&] { return Has(a, "You are now an operator", kInfo) && Has(b, "Server made Alice an operator", kInfo); }),
          "the console makes Alice an operator, and everyone hears about it");

    std::cout << "Commands" << std::endl;
    a.client.SendChat("/msg bob psst");
    Check(both(20.0, [&] { return Has(b, "Alice whispers: psst", kPrivate) && Has(a, "You whisper to Bob: psst", kPrivate); }),
          "/msg reaches its target (names are case-insensitive) and confirms to the sender");
    b.client.SendChat("/me waves");
    Check(both(20.0, [&] { return Has(a, "* Bob waves"); }), "/me");
    b.client.SendChat("/list");
    Check(both(20.0, [&] { return Has(b, "2 players online") && !Has(a, "2 players online"); }), "/list answers only whoever asked");

    a.client.SendChat("/time midnight");
    Check(both(20.0, [&] { return std::abs(b.client.GetTimeOfDay() - 0.75f) < 0.01f && Has(b, "set the time to 00:00"); }),
          "/time changes everyone's sky, and says so");
    a.client.SendChat("/time 13:30");
    Check(both(20.0, [&] { return std::abs(a.client.GetTimeOfDay() - (7.5f / 24.0f)) < 0.01f; }), "/time takes HH:MM (sunrise is 06:00)");
    a.client.SendChat("/weather rain");
    Check(both(15.0, [&] { return a.client.GetRain() > 0.5f; }), "/weather rain brings rain to everyone");
    a.client.SendChat("/weather clear");
    Check(both(15.0, [&] { return a.client.GetRain() < 0.01f; }), "/weather clear stops it");

    a.client.SendChat("/tp 100 20 -50");
    std::optional<glm::vec3> moved;
    Check(both(20.0, [&] { return (moved = a.client.TakeCorrection()).has_value(); }) && glm::distance(*moved, glm::vec3(100, 20, -50)) < 0.01f,
          "/tp x y z moves you there");
    a.client.SendChat("/tp ~ ~5 ~2");
    Check(both(20.0, [&] { return (moved = a.client.TakeCorrection()).has_value(); }) && glm::distance(*moved, glm::vec3(100, 25, -48)) < 0.01f,
          "/tp with ~ is relative to where you are");
    a.client.SendChat("/tp Bob 0 30 0");
    Check(both(20.0, [&] { return (moved = b.client.TakeCorrection()).has_value(); }) && glm::distance(*moved, glm::vec3(0, 30, 0)) < 0.01f &&
              Has(b, "Alice teleported you"),
          "/tp <player> x y z moves them, and tells them");
    a.client.SendChat("/tp Bob");
    Check(both(20.0, [&] { return (moved = a.client.TakeCorrection()).has_value(); }) && glm::distance(*moved, glm::vec3(0, 30, 0)) < 0.01f,
          "/tp <player> takes you to them");

    a.client.SendChat("/frobnicate");
    Check(both(20.0, [&] { return Has(a, "Unknown command /frobnicate", kError); }) && !Has(b, "frobnicate"),
          "an unknown command gets an error, only for its sender");
    a.client.SendChat("/tp nobody");
    Check(both(20.0, [&] { return Has(a, "No player called nobody", kError); }), "a missing player is reported");
    b.client.SendChat("/kick Alice");
    Check(both(20.0, [&] { return Has(b, "Only operators can do that", kError); }) && a.client.IsConnected(), "a player can't /kick");
    a.client.SendChat("/stop");
    Check(both(20.0, [&] { return Has(a, "only works from the server console", kError); }), "even an operator can't /stop from chat");

    std::cout << "Operators" << std::endl;
    a.client.SendChat("/op bob");
    Check(both(20.0, [&] { return Has(b, "You are now an operator"); }), "an operator can make another");
    b.client.SendChat("/deop Alice");
    Check(both(20.0, [&] { return Has(a, "You are no longer an operator"); }), "and an operator can unmake one");
    a.client.SendChat("/weather rain");
    Check(both(20.0, [&] { return Has(a, "Only operators can do that (/weather rain)", kError); }), "Alice can't change the weather any more");
    a.client.SendChat("/ops");
    Check(both(20.0, [&] { return Has(a, "Operators: bob", kInfo); }), "/ops lists the operators");
    const std::string opsFile = ReadFile(dir / "ops.txt");
    Check(opsFile.find("\nbob\n") != std::string::npos && opsFile.find("alice") == std::string::npos, "ops.txt holds bob and not alice");

    std::cout << "Console" << std::endl;
    server.QueueConsoleCommand("time noon");
    Check(both(20.0, [&] { return std::abs(a.client.GetTimeOfDay() - 0.25f) < 0.01f && Has(a, "Server set the time to 12:00"); }),
          "a console command runs without the slash");
    server.QueueConsoleCommand("/msg alice from the console");
    Check(both(20.0, [&] { return Has(a, "Server whispers: from the console", kPrivate); }), "and with it");
    server.QueueConsoleCommand("kick Bob being tested");
    Check(both(20.0, [&] { return !b.client.IsConnected() && Has(a, "Bob was kicked (being tested)", kInfo); }),
          "the console can /kick, and it's announced with the reason");

    a.client.Disconnect();
    server.Stop();

    std::cout << "Restart and owner" << std::endl;
    {
        Server again;
        options.ownerName = "Carol";
        if (!again.Start(options)) return EXIT_FAILURE;
        again.RunInBackground();
        TestClient bob, carol;
        bob.client.Connect("127.0.0.1", again.GetPort(), "Bob", error, kTestViewDistance);
        carol.client.Connect("127.0.0.1", again.GetPort(), "Carol", error, kTestViewDistance);
        auto pump = [&](double s, const std::function<bool()>& done) { return Pump({ &bob, &carol }, s, done); };
        Check(pump(20.0, [&] { return bob.client.IsWelcomed() && Has(bob, "You are an operator"); }), "Bob is still an operator after a restart");
        Check(pump(20.0, [&] { return Has(carol, "You are an operator"); }), "the owner is an operator without being in ops.txt");
        bob.client.SendChat("/deop Carol");
        Check(pump(20.0, [&] { return Has(bob, "owns this server", kError); }), "and can't be removed");
        bob.client.Disconnect();
        carol.client.Disconnect();
        again.Stop();
    }
    std::filesystem::remove_all(dir);
    std::cout << (failures ? "CHAT TEST FAILED (" : "ALL CHAT CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
