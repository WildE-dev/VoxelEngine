cmake_minimum_required(VERSION 3.16.0 FATAL_ERROR)

if(CMAKE_HOST_WIN32)
    set(CMAKE_SYSTEM_VERSION 10.0 CACHE STRING "")
endif()

project(VoxelEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(unofficial-enet CONFIG REQUIRED)
find_package(Threads REQUIRED)

find_path(STB_INCLUDE_DIRS "stb_image.h")
if(NOT STB_INCLUDE_DIRS)
    message(FATAL_ERROR "stb_image.h not found (install the 'stb' vcpkg port)")
endif()

# Shared by client and server, no GL: blocks, chunks, generation, lighting,
# collision, and the network protocol and transport.
add_library(voxel_common STATIC
    src/net/Protocol.cpp
    src/net/Protocol.h
    src/net/Transport.cpp
    src/net/Transport.h

    src/world/Block.cpp
    src/world/Block.h
    src/world/BlockAccess.h
    src/world/Chunk.cpp
    src/world/Chunk.h
    src/world/Caves.cpp
    src/world/Caves.h
    src/world/Collision.cpp
    src/world/Collision.h
    src/world/Coords.h
    src/world/Features.cpp
    src/world/Features.h
    src/world/Lighting.cpp
    src/world/Lighting.h
    src/world/Noise.cpp
    src/world/Noise.h
    src/world/Raycast.cpp
    src/world/Raycast.h
    src/world/TerrainGenerator.cpp
    src/world/TerrainGenerator.h
)
target_include_directories(voxel_common PUBLIC src)
target_link_libraries(voxel_common PUBLIC glm::glm unofficial::enet::enet Threads::Threads)

# The authoritative server: world simulation (water, light, generation) and
# streaming to clients. Headless.
add_library(voxel_server STATIC
    src/server/Server.cpp
    src/server/Server.h
    src/server/ServerWorld.cpp
    src/server/ServerWorld.h
    src/server/Water.cpp
    src/server/Water.h
)
target_link_libraries(voxel_server PUBLIC voxel_common)

# The client's world copy, meshing and connection. Uses GL for chunk meshes.
add_library(voxel_client STATIC
    src/client/ChunkMesher.cpp
    src/client/ChunkMesher.h
    src/client/Client.cpp
    src/client/Client.h
    src/client/ClientChunk.cpp
    src/client/ClientChunk.h
    src/client/ClientWorld.cpp
    src/client/ClientWorld.h
    src/render/Shader.cpp
    src/render/Shader.h
)
target_link_libraries(voxel_client PUBLIC voxel_common glad::glad)

# The game. Also runs a dedicated server with --server (no window, no GL).
add_executable(${PROJECT_NAME}
    src/main.cpp

    src/app/Application.cpp
    src/app/Application.h
    src/app/Camera.cpp
    src/app/Camera.h
    src/app/Player.cpp
    src/app/Player.h

    src/render/DebugDraw.cpp
    src/render/DebugDraw.h
    src/render/PlayerModels.cpp
    src/render/PlayerModels.h
    src/render/Renderer.cpp
    src/render/Renderer.h
    src/render/Textures.cpp
    src/render/Textures.h
    src/render/TextureAtlas.cpp
    src/render/TextureAtlas.h
)

target_include_directories(${PROJECT_NAME} PRIVATE ${STB_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE voxel_server voxel_client glfw imgui::imgui)

# A dedicated server binary that doesn't link GL or GLFW at all.
add_executable(VoxelServer src/server/main.cpp)
target_link_libraries(VoxelServer PRIVATE voxel_server)

# Starts a server and two network clients in one process and checks that chunks,
# edits, water flow and player positions reach both clients.
add_executable(NetSmokeTest tests/NetSmokeTest.cpp src/app/Player.cpp)
target_link_libraries(NetSmokeTest PRIVATE voxel_server voxel_client)

foreach(target voxel_common voxel_server voxel_client ${PROJECT_NAME} VoxelServer NetSmokeTest)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endforeach()

# Copy shaders/textures next to the executable on every build so edits are picked up.
# The app loads them relative to the working directory.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/Resources $<TARGET_FILE_DIR:${PROJECT_NAME}>/Resources
    COMMENT "Copying resources")

set_target_properties(${PROJECT_NAME} PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${PROJECT_NAME}>)
