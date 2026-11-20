// Dedicated server without any GL or windowing dependency:
//   VoxelServer [port] [--world DIR] [--no-save] [--water-distance N] [--empty-world]
//               [--weather clear|rain|auto] [--day-length SECONDS] [--op NAME]...
//               [--mode creative|survival]
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "net/Protocol.h"
#include "server/Server.h"

namespace {
Server* runningServer = nullptr;
void StopServer(int) {
    if (runningServer) runningServer->Stop();
}
// Reads commands typed into the console and hands them to the server. Detached: it may
// still be waiting on input when the server stops.
void StartConsole(Server& server) {
    std::thread([&server] {
        for (std::string line; std::getline(std::cin, line);) server.QueueConsoleCommand(line);
    }).detach();
}
} // namespace

int main(int argc, char** argv) {
    Server::Options options;
    options.worldDirectory = "world";
    bool worldGiven = false, modeGiven = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--world" && i + 1 < argc) {
            options.worldDirectory = argv[++i];
            worldGiven = true;
            continue;
        }
        if (arg == "--op" && i + 1 < argc) {
            options.operators.push_back(argv[++i]);
            continue;
        }
        if (arg == "--weather" && i + 1 < argc) {
            const std::string w = argv[++i];
            if (w == "auto") options.weather = Server::Options::Weather::Auto;
            else if (w == "clear") options.weather = Server::Options::Weather::Clear;
            else if (w == "rain") options.weather = Server::Options::Weather::Rain;
            else { std::cerr << "Weather must be clear, rain or auto" << std::endl; return EXIT_FAILURE; }
            continue;
        }
        if (arg == "--day-length" && i + 1 < argc) {
            options.dayLengthSeconds = std::max(1.0f, static_cast<float>(std::atof(argv[++i])));
            continue;
        }
        if (arg == "--mode" && i + 1 < argc) {
            const std::string m = argv[++i];
            if (m == "creative") options.defaultMode = net::GameMode::Creative;
            else if (m == "survival") options.defaultMode = net::GameMode::Survival;
            else { std::cerr << "Mode must be creative or survival" << std::endl; return EXIT_FAILURE; }
            modeGiven = true;
            continue;
        }
        if (arg == "--empty-world") {
            options.worldType = TerrainGenerator::Type::Empty;
            if (!modeGiven) options.defaultMode = net::GameMode::Creative;
            if (!worldGiven && !options.worldDirectory.empty()) options.worldDirectory = "world-empty";
            continue;
        }
        if (arg == "--no-save") {
            options.worldDirectory.clear();
            continue;
        }
        if (arg == "--water-distance" && i + 1 < argc) {
            options.waterSimulationDistance = std::atoi(argv[++i]);
            continue;
        }
        const long port = std::strtol(arg.c_str(), nullptr, 10);
        if (port <= 0 || port > 65535) {
            std::cerr << "Usage: " << argv[0] << " [port] [--world DIR] [--no-save]\n"
                      << "  port      default " << net::kDefaultPort << "\n"
                      << "  --world   where the world is saved (default: world)\n"
                      << "  --no-save don't load or save the world\n"
                      << "  --water-distance N  chunks around players where water flows (default 6)\n"
                      << "  --empty-world  a new world that's a flat, empty plain (default save: world-empty)\n"
                      << "  --weather W    clear, rain or auto (default)\n"
                      << "  --day-length S seconds in a day (default 1200)\n"
                      << "  --op NAME      make a player an operator (repeatable; kept in ops.txt)\n"
                      << "  --mode M       creative or survival: a new world's game mode (default survival)" << std::endl;
            return EXIT_FAILURE;
        }
        options.port = static_cast<uint16_t>(port);
    }
    Server server;
    if (!server.Start(options)) return EXIT_FAILURE;
    runningServer = &server;
    StartConsole(server);
    std::cout << "[server] type commands here (help lists them)" << std::endl;
    std::signal(SIGINT, StopServer);
    std::signal(SIGTERM, StopServer);
    server.Run();
    std::cout << "[server] stopped" << std::endl;
    return EXIT_SUCCESS;
}
