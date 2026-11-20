// Dedicated server without any GL or windowing dependency:
//   VoxelServer [port] [--world DIR] [--no-save] [--water-distance N] [--empty-world]
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#include "net/Protocol.h"
#include "server/Server.h"

namespace {
Server* runningServer = nullptr;
void StopServer(int) {
    if (runningServer) runningServer->Stop();
}
} // namespace

int main(int argc, char** argv) {
    Server::Options options;
    options.worldDirectory = "world";
    bool worldGiven = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--world" && i + 1 < argc) {
            options.worldDirectory = argv[++i];
            worldGiven = true;
            continue;
        }
        if (arg == "--empty-world") {
            options.worldType = TerrainGenerator::Type::Empty;
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
                      << "  --empty-world  a new world that's a flat, empty plain (default save: world-empty)" << std::endl;
            return EXIT_FAILURE;
        }
        options.port = static_cast<uint16_t>(port);
    }
    Server server;
    if (!server.Start(options)) return EXIT_FAILURE;
    runningServer = &server;
    std::signal(SIGINT, StopServer);
    std::signal(SIGTERM, StopServer);
    server.Run();
    std::cout << "[server] stopped" << std::endl;
    return EXIT_SUCCESS;
}
