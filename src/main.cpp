#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "app/Application.h"
#include "net/Protocol.h"
#include "server/Server.h"

namespace {

Server* runningServer = nullptr;

void StopServer(int) {
    if (runningServer) runningServer->Stop();
}

void PrintUsage(const char* program) {
    std::cout << "Usage:\n"
              << "  " << program << "                      the title screen: pick or make a world, join a server, settings\n"
              << "  " << program << " --server [port]      run a dedicated server, no window (default port "
              << net::kDefaultPort << ")\n"
              << "  " << program << " --connect host[:port] join a server\n"
              << "Options:\n"
              << "  --name NAME   your player name (saved worlds remember players by name)\n"
              << "  --world DIR   where the world is saved (default: saves/singleplayer, or world\n"
              << "                with --server)\n"
              << "  --no-save     don't load or save the world\n"
              << "  --op NAME     make a player an operator (server; repeatable, kept in ops.txt)\n"
              << "  --weather W   clear, rain, or auto (default: rain comes and goes)\n"
              << "  --day-length S  seconds in a day (default 1200)\n"
              << "  --view-distance N  server: chunks loaded and sent around each player (2-12)\n"
              << "  --mode M      creative or survival: a new world's game mode (default survival,\n"
              << "                creative for an empty world)\n"
              << "  --empty-world a new world that's a flat, empty plain, for testing (default\n"
              << "                save: saves/empty, or world-empty with --server)\n"
              << "  --water-distance N  chunks around players where water flows (default 6, below\n"
              << "                the view distance of 12)\n";
}

bool ParsePort(const std::string& text, uint16_t& port) {
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || value <= 0 || value > 65535) return false;
    port = static_cast<uint16_t>(value);
    return true;
}

// --weather clear|rain|auto
// --mode creative|survival
bool ParseMode(const std::string& text, int& mode) {
    if (text == "creative") mode = static_cast<int>(net::GameMode::Creative);
    else if (text == "survival") mode = static_cast<int>(net::GameMode::Survival);
    else return false;
    return true;
}

bool ParseWeather(const std::string& text, Server::Options::Weather& weather) {
    if (text == "auto") weather = Server::Options::Weather::Auto;
    else if (text == "clear") weather = Server::Options::Weather::Clear;
    else if (text == "rain") weather = Server::Options::Weather::Rain;
    else return false;
    return true;
}

// Reads commands typed into the console and hands them to the server. Detached: it may
// still be waiting on input when the server stops.
void StartConsole(Server& server) {
    std::thread([&server] {
        for (std::string line; std::getline(std::cin, line);) server.QueueConsoleCommand(line);
    }).detach();
}

int RunServer(uint16_t port, const std::string& worldDirectory, int waterDistance, bool emptyWorld, Server::Options sky) {
    Server server;
    Server::Options options;
    options.dayLengthSeconds = sky.dayLengthSeconds;
    options.operators = sky.operators;
    options.weather = sky.weather;
    options.defaultMode = sky.defaultMode;
    options.viewDistance = sky.viewDistance;
    if (emptyWorld) options.worldType = TerrainGenerator::Type::Empty;
    options.waterSimulationDistance = waterDistance;
    options.port = port;
    options.worldDirectory = worldDirectory;
    if (!server.Start(options)) return EXIT_FAILURE;
    runningServer = &server;
    StartConsole(server);
    std::cout << "[server] type commands here (help lists them)" << std::endl;
    std::signal(SIGINT, StopServer);
    std::signal(SIGTERM, StopServer);
    server.Run();
    runningServer = nullptr;
    std::cout << "[server] stopped" << std::endl;
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv)
{
    ClientOptions client;
    bool dedicated = false, noSave = false, emptyWorld = false;
    Server::Options sky; // day length and weather
    int waterDistance = Server::Options{}.waterSimulationDistance;
    std::string worldDirectory;
    uint16_t serverPort = net::kDefaultPort;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            dedicated = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                if (!ParsePort(argv[++i], serverPort)) {
                    std::cerr << "Invalid port '" << argv[i] << "'" << std::endl;
                    return EXIT_FAILURE;
                }
            }
        }
        else if (arg == "--world" && i + 1 < argc) {
            worldDirectory = argv[++i];
        }
        else if (arg == "--no-save") {
            noSave = true;
        }
        else if (arg == "--empty-world") {
            emptyWorld = true;
        }
        else if (arg == "--op" && i + 1 < argc) {
            sky.operators.push_back(argv[++i]);
        }
        else if (arg == "--weather" && i + 1 < argc) {
            if (!ParseWeather(argv[++i], sky.weather)) {
                std::cerr << "Weather must be clear, rain or auto" << std::endl;
                return EXIT_FAILURE;
            }
        }
        else if (arg == "--day-length" && i + 1 < argc) {
            sky.dayLengthSeconds = static_cast<float>(std::atof(argv[++i]));
            if (sky.dayLengthSeconds < 1.0f) {
                std::cerr << "Day length must be at least a second" << std::endl;
                return EXIT_FAILURE;
            }
        }
        else if (arg == "--view-distance" && i + 1 < argc) {
            sky.viewDistance = std::clamp(std::atoi(argv[++i]), 2, 12);
        }
        else if (arg == "--mode" && i + 1 < argc) {
            if (!ParseMode(argv[++i], client.mode)) {
                std::cerr << "Mode must be creative or survival" << std::endl;
                return EXIT_FAILURE;
            }
        }
        else if (arg == "--water-distance" && i + 1 < argc) {
            waterDistance = std::atoi(argv[++i]);
        }
        else if (arg == "--connect" && i + 1 < argc) {
            std::string target = argv[++i];
            const size_t colon = target.rfind(':');
            if (colon != std::string::npos) {
                if (!ParsePort(target.substr(colon + 1), client.port)) {
                    std::cerr << "Invalid port in '" << target << "'" << std::endl;
                    return EXIT_FAILURE;
                }
                target = target.substr(0, colon);
            }
            client.host = target;
        }
        else if (arg == "--name" && i + 1 < argc) {
            client.name = argv[++i];
            client.nameGiven = true;
        }
        else {
            PrintUsage(argv[0]);
            return arg == "--help" || arg == "-h" ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }
    // An empty world gets its own default save, so it never opens the normal one.
    if (worldDirectory.empty()) worldDirectory = emptyWorld ? (dedicated ? "world-empty" : "saves/empty") : (dedicated ? "world" : "");
    sky.defaultMode = client.mode >= 0 ? static_cast<net::GameMode>(client.mode)
                      : emptyWorld ? net::GameMode::Creative : net::GameMode::Survival;
    if (dedicated) return RunServer(serverPort, noSave ? "" : worldDirectory, waterDistance, emptyWorld, sky);
    client.dayLengthSeconds = sky.dayLengthSeconds;
    client.weather = static_cast<int>(sky.weather);
    client.emptyWorld = emptyWorld;
    client.waterSimulationDistance = waterDistance;
    if (!worldDirectory.empty()) client.worldDirectory = worldDirectory;
    if (noSave) client.worldDirectory.clear();
    if (!client.host.empty() && client.port == 0) client.port = net::kDefaultPort;
    // No options at all: the title screen, to pick a world or a server there.
    client.showMenu = argc == 1;

    Application app;
    if (!app.Init(client)) return EXIT_FAILURE;
    app.Run();
    return EXIT_SUCCESS;
}
