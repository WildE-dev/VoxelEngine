// Dedicated server without any GL or windowing dependency:
//   VoxelServer [port]
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
    if (argc > 1) {
        const long port = std::strtol(argv[1], nullptr, 10);
        if (port <= 0 || port > 65535) {
            std::cerr << "Usage: " << argv[0] << " [port]   (default " << net::kDefaultPort << ")" << std::endl;
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
