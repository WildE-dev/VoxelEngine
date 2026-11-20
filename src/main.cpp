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
              << "  --name NAME   your player name\n";
}

bool ParsePort(const std::string& text, uint16_t& port) {
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || value <= 0 || value > 65535) return false;
    port = static_cast<uint16_t>(value);
    return true;
}

int RunServer(uint16_t port) {
    Server server;
    Server::Options options;
    options.port = port;
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
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            uint16_t port = net::kDefaultPort;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                if (!ParsePort(argv[++i], port)) {
                    std::cerr << "Invalid port '" << argv[i] << "'" << std::endl;
                    return EXIT_FAILURE;
                }
            }
            return RunServer(port);
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
    if (!client.host.empty() && client.port == 0) client.port = net::kDefaultPort;

    Application app;
    if (!app.Init(client)) return EXIT_FAILURE;
    app.Run();
    return EXIT_SUCCESS;
}
