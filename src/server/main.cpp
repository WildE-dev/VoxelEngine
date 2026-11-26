// Dedicated server without any GL or windowing dependency:
//   VoxelServer [port] [--world DIR] [--no-save] [--water-distance N] [--empty-world]
//               [--weather clear|rain|auto] [--day-length SECONDS] [--op NAME]...
//               [--mode creative|survival] [--relay HOST[:PORT]] [--lan]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "net/Protocol.h"
#include "net/Lan.h"
#include "net/RelayHost.h"
#include "server/Server.h"

namespace {
Server* runningServer = nullptr;
void StopServer(int) {
    if (runningServer) runningServer->Stop();
}

// The console window closed (or Ctrl+Break, logging off, shutting down), on Windows:
// the process is ended as soon as this returns, so it stops the server and waits here
// (on the thread Windows runs it on) for the last save, a little short of the five
// seconds Windows allows.
#ifdef SIGBREAK
std::atomic<bool> serverDone{false};
void StopServerAndWait(int) {
    if (runningServer) runningServer->Stop();
    for (int i = 0; i < 450 && !serverDone; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
#endif
// Reads commands typed into the console and hands them to the server. Detached: it may
// still be waiting on input when the server stops.
void StartConsole(Server& server) {
    std::thread([&server] {
        for (std::string line; std::getline(std::cin, line);) server.QueueConsoleCommand(line);
    }).detach();
}
} // namespace

int main(int argc, char** argv) {
    std::string relayAddress;
    bool announceOnLan = false;
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
            else if (w == "storm") options.weather = Server::Options::Weather::Storm;
            else { std::cerr << "Weather must be clear, rain, storm or auto" << std::endl; return EXIT_FAILURE; }
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
        if (arg == "--lan") {
            announceOnLan = true;
            continue;
        }
        if (arg == "--relay" && i + 1 < argc) {
            relayAddress = argv[++i];
            continue;
        }
        if (arg == "--no-save") {
            options.worldDirectory.clear();
            continue;
        }
        if (arg == "--view-distance" && i + 1 < argc) {
            options.viewDistance = std::clamp(std::atoi(argv[++i]), 2, 12);
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
                      << "  --mode M       creative or survival: a new world's game mode (default survival)\n"
                      << "  --view-distance N  chunks loaded and sent around each player (2-12, default 12)\n"
                      << "  --relay HOST[:PORT]  also open it to friends through that relay (VoxelRelay): prints a join code\n"
                      << "  --lan          announce it on the local network (listed on the Multiplayer page)" << std::endl;
            return EXIT_FAILURE;
        }
        options.port = static_cast<uint16_t>(port);
    }
    Server server;
    if (!server.Start(options)) return EXIT_FAILURE;
    runningServer = &server;
    StartConsole(server);
    std::cout << "[server] type commands here (help lists them)" << std::endl;
    // Opened to friends through a relay: the join code is printed once the relay gives it.
    net::RelayHost relayHost;
    std::atomic<bool> serverRunning{ true };
    std::thread relayReporter;
    if (!relayAddress.empty()) {
        std::string error;
        if (!relayHost.Start(relayAddress, server.GetPort(), error)) std::cerr << "[relay] " << error << std::endl;
        else relayReporter = std::thread([&] {
            std::string said;
            while (serverRunning) {
                const std::string now = relayHost.GetState() == net::RelayHost::State::Open ? "join code: " + relayHost.GetCode() : relayHost.GetStatus();
                if (now != said) std::cout << "[relay] " << (said = now) << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        });
    }
    // Announced on the local network: listed on other machines' Multiplayer pages.
    net::LanHost lanHost;
    if (announceOnLan) {
        std::string error;
        const std::string world = options.worldDirectory.empty() ? "unsaved world" : options.worldDirectory;
        if (!lanHost.Start("Server", world, server.GetPort(), false, error)) std::cerr << "[lan] " << error << std::endl;
        else std::cout << "[lan] announced on the local network" << std::endl;
    }
    std::signal(SIGINT, StopServer);
    std::signal(SIGTERM, StopServer);
#ifdef SIGBREAK
    std::signal(SIGBREAK, StopServerAndWait);
#endif
    server.Run();
    runningServer = nullptr;
    serverRunning = false;
    if (relayReporter.joinable()) relayReporter.join();
    relayHost.Stop();
    lanHost.Stop();
#ifdef SIGBREAK
    serverDone = true;
#endif
    std::cout << "[server] stopped" << std::endl;
    return EXIT_SUCCESS;
}
