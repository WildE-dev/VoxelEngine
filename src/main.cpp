#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

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
              << "  " << program << "                      play singleplayer (runs a local server)\n"
              << "  " << program << " --server [port]      run a dedicated server, no window (default port "
              << net::kDefaultPort << ")\n"
              << "  " << program << " --connect host[:port] join a server\n"
              << "Options:\n"
              << "  --name NAME   your player name (saved worlds remember players by name)\n"
              << "  --world DIR   where the world is saved (default: saves/singleplayer, or world\n"
              << "                with --server)\n"
              << "  --no-save     don't load or save the world\n"
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

int RunServer(uint16_t port, const std::string& worldDirectory, int waterDistance) {
    Server server;
    Server::Options options;
    options.waterSimulationDistance = waterDistance;
    options.port = port;
    options.worldDirectory = worldDirectory;
    if (!server.Start(options)) return EXIT_FAILURE;
    runningServer = &server;
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
    bool dedicated = false, noSave = false;
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
        else if (arg == "--water-distance" && i + 1 < argc) {
            waterDistance = std::atoi(argv[++i]);
        }
        if (arg == "--connect" && i + 1 < argc) {
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
        }
        else {
            PrintUsage(argv[0]);
            return arg == "--help" || arg == "-h" ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }
    if (dedicated) return RunServer(serverPort, noSave ? "" : worldDirectory.empty() ? "world" : worldDirectory, waterDistance);
    client.waterSimulationDistance = waterDistance;
    if (!worldDirectory.empty()) client.worldDirectory = worldDirectory;
    if (noSave) client.worldDirectory.clear();
    if (!client.host.empty() && client.port == 0) client.port = net::kDefaultPort;

    Application app;
    if (!app.Init(client)) return EXIT_FAILURE;
    app.Run();
    return EXIT_SUCCESS;
}
