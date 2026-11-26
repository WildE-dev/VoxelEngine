// VoxelRelay: the relay that lets friends join a game opened to them, by join code.
// See README.md, "Playing with friends over the internet", for running it on a VPS.
//   voxel-relay [--port N] [--bind ADDRESS] [--max-rooms N] [--quiet]
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "relay/RelayServer.h"

int main(int argc, char** argv) {
    relay::RelayConfig config;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--port" && hasValue) config.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--bind" && hasValue) config.bindHost = argv[++i];
        else if (arg == "--max-rooms" && hasValue) config.maxRooms = std::atoi(argv[++i]);
        else if (arg == "--quiet") config.log = false;
        else {
            std::cout << "usage: voxel-relay [--port N (default 25700)] [--bind ADDRESS] [--max-rooms N] [--quiet]" << std::endl;
            return arg == "--help" || arg == "-h" ? 0 : 1;
        }
    }
    relay::RelayServer server;
    std::string error;
    if (!server.Start(config, error)) {
        std::cerr << "voxel-relay: " << error << std::endl;
        return 1;
    }
    for (;;) server.RunOnce(1000);
}
