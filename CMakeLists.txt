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
    src/world/Dungeons.cpp
    src/world/Dwarves.cpp
    src/world/Json.cpp
    src/world/BlockData.cpp
    src/world/Multiblocks.cpp
    src/world/Dungeons.h
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
    src/world/Power.h
    src/world/Signals.h
)
target_include_directories(voxel_common PUBLIC src)
target_link_libraries(voxel_common PUBLIC glm::glm unofficial::enet::enet Threads::Threads)

# The authoritative server: world simulation (water, light, generation) and
# streaming to clients. Headless.
add_library(voxel_server STATIC
    src/server/Creatures.cpp
    src/server/Creatures.h
    src/server/Server.cpp
    src/server/Power.cpp
    src/server/Signals.cpp
    src/server/Miners.cpp
    src/server/Multiblocks.cpp
    src/server/Server.h
    src/server/ServerCommands.cpp
    src/server/ServerWorld.cpp
    src/server/ServerWorld.h
    src/server/Water.cpp
    src/server/Water.h
    src/server/WorldStorage.cpp
    src/server/WorldStorage.h
)
target_link_libraries(voxel_server PUBLIC voxel_common)

# The client's world copy, meshing and connection. Uses GL for chunk meshes.
add_library(voxel_client STATIC
    src/audio/Audio.cpp
    src/audio/Audio.h
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
# Sound through OpenAL: Apple's framework on macOS, otherwise an installed OpenAL
# (e.g. openal-soft) if there is one. Without it the game is simply silent.
if(APPLE)
    target_link_libraries(voxel_client PUBLIC "-framework OpenAL")
    target_compile_definitions(voxel_client PUBLIC VOXEL_AUDIO=1)
    target_compile_options(voxel_client PRIVATE -Wno-deprecated-declarations)
else()
    find_package(OpenAL QUIET)
    if(OpenAL_FOUND)
        target_link_libraries(voxel_client PUBLIC OpenAL::OpenAL)
        target_compile_definitions(voxel_client PUBLIC VOXEL_AUDIO=1)
    endif()
endif()

# The game. Also runs a dedicated server with --server (no window, no GL).
add_executable(${PROJECT_NAME}
    src/main.cpp

    src/app/Application.cpp
    src/app/Application.h
    src/app/Camera.cpp
    src/app/Camera.h
    src/app/Menu.cpp
    src/app/Player.cpp
    src/app/Player.h

    src/render/DebugDraw.cpp
    src/render/DebugDraw.h
    src/render/PlayerModels.cpp
    src/render/PlayerModels.h
    src/render/Rain.cpp
    src/render/Rain.h
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

# Saves a world, restarts, and checks edits, light and players come back.
add_executable(SaveTest tests/SaveTest.cpp src/app/Player.cpp)
target_link_libraries(SaveTest PRIVATE voxel_server voxel_client)

# Water only flows within the simulation distance of players.
add_executable(WaterAreaTest tests/WaterAreaTest.cpp)
target_link_libraries(WaterAreaTest PRIVATE voxel_server)

# Pumps move water through pipes, conserving it.
add_executable(SignalTest tests/SignalTest.cpp)
target_link_libraries(SignalTest PRIVATE voxel_server voxel_client)
add_executable(DataTest tests/DataTest.cpp)
target_link_libraries(DataTest PRIVATE voxel_common)
add_executable(FactoryTest tests/FactoryTest.cpp)
target_link_libraries(FactoryTest PRIVATE voxel_server voxel_client)
add_executable(PowerTest tests/PowerTest.cpp)
target_link_libraries(PowerTest PRIVATE voxel_server voxel_client)
add_executable(PumpTest tests/PumpTest.cpp)
target_link_libraries(PumpTest PRIVATE voxel_server)

# Empty worlds, and puddles drying up.
add_executable(EvaporationTest tests/EvaporationTest.cpp)
target_link_libraries(EvaporationTest PRIVATE voxel_server)

# Thin water, rain, and the day/night clock.
add_executable(WeatherTest tests/WeatherTest.cpp)
target_link_libraries(WeatherTest PRIVATE voxel_server voxel_client)

# Chat and commands between a server and two clients.
add_executable(ChatTest tests/ChatTest.cpp)
target_link_libraries(ChatTest PRIVATE voxel_server voxel_client)

# Dungeon layouts and generation.
add_executable(DungeonTest tests/DungeonTest.cpp)
target_link_libraries(DungeonTest PRIVATE voxel_common)

# Traps, health, no-clip and the laser between a server and two clients.
add_executable(TrapTest tests/TrapTest.cpp src/app/Player.cpp)
target_link_libraries(TrapTest PRIVATE voxel_server voxel_client)

# Sound: the sound files, materials, and hearing others' edits.
add_executable(SoundTest tests/SoundTest.cpp)
target_link_libraries(SoundTest PRIVATE voxel_server voxel_client)

# Crafting: recipes, tools made once and kept, glass.
add_executable(CraftTest tests/CraftTest.cpp)
target_link_libraries(CraftTest PRIVATE voxel_server voxel_client)

# Creatures: summoning, fights, ghouls, burning, natural spawning.
add_executable(CreatureTest tests/CreatureTest.cpp)
target_link_libraries(CreatureTest PRIVATE voxel_server voxel_client)

# Mining: hardness, tools, and the server timing breaks.
add_executable(MiningTest tests/MiningTest.cpp)
target_link_libraries(MiningTest PRIVATE voxel_server voxel_client)

# Food: hunger, starving, eating, venison from deer and cooking it.
add_executable(FoodTest tests/FoodTest.cpp)
target_link_libraries(FoodTest PRIVATE voxel_server voxel_client)

# Chests: storing, sharing, breaking and saving.
add_executable(ChestTest tests/ChestTest.cpp)
target_link_libraries(ChestTest PRIVATE voxel_server voxel_client)

# Beds: respawning at your bed, and sleeping through the night.
add_executable(BedTest tests/BedTest.cpp)
target_link_libraries(BedTest PRIVATE voxel_server voxel_client)

# Breathing: drowning, thin air up mountains, stale air deep down.
add_executable(BreathTest tests/BreathTest.cpp)
target_link_libraries(BreathTest PRIVATE voxel_server voxel_client)

# Creative and survival: resources, costs, /gamemode, /give, saving.
add_executable(ModeTest tests/ModeTest.cpp src/app/Player.cpp)
target_link_libraries(ModeTest PRIVATE voxel_server voxel_client)

# Block shapes (sloped and stepped), the chisel, joined slopes, thin water, mountains.
add_executable(ShapeTest tests/ShapeTest.cpp src/app/Player.cpp)
target_link_libraries(ShapeTest PRIVATE voxel_server voxel_client)

# Biomes: climate, ground, plants, ice and swamps.
add_executable(BiomeTest tests/BiomeTest.cpp)
target_link_libraries(BiomeTest PRIVATE voxel_common)

foreach(target voxel_common voxel_server voxel_client ${PROJECT_NAME} VoxelServer NetSmokeTest SaveTest WaterAreaTest PumpTest EvaporationTest WeatherTest ChatTest DungeonTest TrapTest ShapeTest BiomeTest ModeTest BreathTest MiningTest CreatureTest CraftTest SoundTest FoodTest ChestTest BedTest PowerTest SignalTest FactoryTest DataTest)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endforeach()

# Copy shaders/textures next to the executable on every build so edits are picked up,
# even when only a resource changed (a post-build step would only run when the game
# relinks). It's part of ALL and runs after the game is built (the generator expression
# makes it depend on the game). The app loads them relative to the working directory.
add_custom_target(copy_resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/Resources $<TARGET_FILE_DIR:${PROJECT_NAME}>/Resources
    COMMENT "Copying resources")

set_target_properties(${PROJECT_NAME} PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${PROJECT_NAME}>)

# `ctest` (or `ctest -j14` to run them all at once) runs every test suite.
enable_testing()
foreach(test NetSmokeTest SaveTest WaterAreaTest PumpTest EvaporationTest WeatherTest ChatTest DungeonTest TrapTest ShapeTest BiomeTest ModeTest BreathTest MiningTest CreatureTest CraftTest SoundTest FoodTest ChestTest BedTest PowerTest SignalTest FactoryTest DataTest)
    add_test(NAME ${test} COMMAND ${test} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endforeach()
