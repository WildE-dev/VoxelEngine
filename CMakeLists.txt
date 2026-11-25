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
    src/world/Items.cpp
    src/world/Items.h
    src/world/Legacy.h
    src/world/Recipes.cpp
    src/world/BlockData.cpp
    src/world/Bodies.cpp
    src/world/Bodies.h
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
# ENet's Windows backend uses Winsock and the multimedia timer.
if(WIN32)
    target_link_libraries(voxel_common PUBLIC ws2_32 winmm)
endif()

# The authoritative server: world simulation (water, light, generation) and
# streaming to clients. Headless.
add_library(voxel_server STATIC
    src/server/Creatures.cpp
    src/server/Creatures.h
    src/server/DroppedItems.cpp
    src/server/DroppedItems.h
    src/server/Steam.cpp
    src/server/Steam.h
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
# (openal-soft, which vcpkg installs on Windows). Without one the game is simply silent.
if(APPLE)
    target_link_libraries(voxel_client PUBLIC "-framework OpenAL")
    target_compile_definitions(voxel_client PUBLIC VOXEL_AUDIO=1)
    target_compile_options(voxel_client PRIVATE -Wno-deprecated-declarations)
else()
    # openal-soft's own package (from vcpkg on Windows) first, then CMake's FindOpenAL.
    find_package(OpenAL CONFIG QUIET)
    if(NOT OpenAL_FOUND)
        find_package(OpenAL QUIET)
    endif()
    if(OpenAL_FOUND)
        target_link_libraries(voxel_client PUBLIC OpenAL::OpenAL)
        target_compile_definitions(voxel_client PUBLIC VOXEL_AUDIO=1)
        message(STATUS "Sound: on (OpenAL)")
    else()
        message(STATUS "Sound: off (no OpenAL found; the game will be silent)")
    endif()
endif()

# The game. Also runs a dedicated server with --server (no window, no GL).
add_executable(${PROJECT_NAME}
    src/main.cpp

    src/app/Application.cpp
    src/app/Application.h
    src/app/Bindings.h
    src/app/Camera.cpp
    src/app/Camera.h
    src/app/ItemPalette.h
    src/app/Menu.cpp
    src/app/Player.cpp
    src/app/Player.h
    src/app/PlayerNames.h
    src/app/Settings.h

    src/render/DebugDraw.cpp
    src/render/DebugDraw.h
    src/render/PlayerModels.cpp
    src/render/PlayerModels.h
    src/render/Rain.cpp
    src/render/Rain.h
    src/render/Renderer.cpp
    src/render/Renderer.h
    src/render/SteamVolume.cpp
    src/render/SteamVolume.h
    src/render/Textures.cpp
    src/render/Textures.h
    src/render/TextureAtlas.cpp
    src/render/TextureAtlas.h

    src/ui/Ui.cpp
    src/ui/Ui.h
)

target_include_directories(${PROJECT_NAME} PRIVATE ${STB_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE voxel_server voxel_client glfw glad::glad)

# The model editor (src/editor/ModelEditor.cpp): its own program, for the creature and
# structure models in Resources/Data, drawn as the game draws them.
add_executable(ModelEditor
    src/editor/ModelEditor.cpp
    src/render/PlayerModels.cpp
    src/render/Textures.cpp
    src/render/TextureAtlas.cpp
)
target_include_directories(ModelEditor PRIVATE ${STB_INCLUDE_DIRS})
target_link_libraries(ModelEditor PRIVATE voxel_client glfw imgui::imgui)
target_compile_definitions(ModelEditor PRIVATE VOXEL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# A dedicated server binary that doesn't link GL or GLFW at all.
add_executable(VoxelServer src/server/main.cpp)
target_link_libraries(VoxelServer PRIVATE voxel_server)

# Warnings for our own code. (MSVC's "unsafe" notes about getenv, localtime and the like
# are off: those are the standard functions, and its replacements for them aren't.)
function(voxel_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()
foreach(target voxel_common voxel_server voxel_client ${PROJECT_NAME} ModelEditor VoxelServer)
    voxel_warnings(${target})
endforeach()

# Where the programs end up: the build directory, or with a multi-configuration
# generator (Visual Studio) a directory per configuration inside it. They load
# Resources/ relative to the working directory, so that's where the resources are
# copied and where the tests run.
get_property(VOXEL_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(VOXEL_MULTI_CONFIG)
    set(VOXEL_RUNTIME_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>")
else()
    set(VOXEL_RUNTIME_DIR "${CMAKE_BINARY_DIR}")
endif()

# Copy shaders, textures and data next to the programs on every build, so edits are
# picked up even when only a resource changed (a post-build step would only run when the
# game relinks). It doesn't wait on the game: while a running game keeps its executable
# from being relinked, the resources (which the tests read too) are still brought up to
# date.
add_custom_target(copy_resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/Resources ${VOXEL_RUNTIME_DIR}/Resources
    COMMENT "Copying resources")

set_target_properties(${PROJECT_NAME} PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${PROJECT_NAME}>)

# `ctest` (or `ctest -j16` to run them all at once) runs every test suite.
enable_testing()

# A test suite, tests/<name>.cpp: built with any other SOURCES, linked to LIBS, and run
# by ctest. The long ones run in parts, side by side: with GROUPS, each group of the
# suite's sections is a ctest test of its own, such as CreatureTest.raids (see
# tests/TestSections.h). Run without a group, a test runs everything.
function(voxel_test name)
    cmake_parse_arguments(TEST "" "" "SOURCES;LIBS;GROUPS" ${ARGN})
    add_executable(${name} tests/${name}.cpp ${TEST_SOURCES})
    target_link_libraries(${name} PRIVATE ${TEST_LIBS})
    voxel_warnings(${name})
    if(TEST_GROUPS)
        foreach(group ${TEST_GROUPS})
            add_test(NAME ${name}.${group} COMMAND ${name} ${group} WORKING_DIRECTORY ${VOXEL_RUNTIME_DIR})
        endforeach()
    else()
        add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${VOXEL_RUNTIME_DIR})
    endif()
endfunction()

# Starts a server and two network clients in one process and checks that chunks,
# edits, water flow and player positions reach both clients.
voxel_test(NetSmokeTest SOURCES src/app/Player.cpp LIBS voxel_server voxel_client)
# Saves a world, restarts, and checks edits, light and players come back.
voxel_test(SaveTest SOURCES src/app/Player.cpp LIBS voxel_server voxel_client)
# Water only flows within the simulation distance of players.
voxel_test(WaterAreaTest LIBS voxel_server)
# Pumps move water through pipes, conserving it.
voxel_test(PumpTest LIBS voxel_server)
# Empty worlds, and puddles drying up.
voxel_test(EvaporationTest LIBS voxel_server)
# Thin water, rain, and the day/night clock.
voxel_test(WeatherTest LIBS voxel_server voxel_client)
# Chat and commands between a server and two clients.
voxel_test(ChatTest LIBS voxel_server voxel_client)
# Dungeon layouts and generation.
voxel_test(DungeonTest LIBS voxel_common)
# Traps, health, no-clip and the laser between a server and two clients.
voxel_test(TrapTest SOURCES src/app/Player.cpp LIBS voxel_server voxel_client GROUPS traps falling tnt)
# Block shapes (sloped and stepped), the chisel, joined slopes, thin water, mountains.
voxel_test(ShapeTest SOURCES src/app/Player.cpp LIBS voxel_server voxel_client)
# Biomes: climate, ground, plants, ice and swamps.
voxel_test(BiomeTest LIBS voxel_common)
# Creative and survival: resources, costs, /gamemode, /give, saving.
voxel_test(ModeTest SOURCES src/app/Player.cpp LIBS voxel_server voxel_client)
# Breathing: drowning, thin air up mountains, stale air deep down.
voxel_test(BreathTest LIBS voxel_server voxel_client GROUPS water air)
# Mining: hardness, tools, and the server timing breaks.
voxel_test(MiningTest LIBS voxel_server voxel_client)
# Creatures: summoning, fights, ghouls, burning, natural spawning.
voxel_test(CreatureTest LIBS voxel_server voxel_client GROUPS fighting allies raids pvp spawning)
# Crafting: recipes, tools made once and kept, glass.
voxel_test(CraftTest LIBS voxel_server voxel_client)
# Sound: the sound files, materials, and hearing others' edits.
voxel_test(SoundTest LIBS voxel_server voxel_client)
# Food: hunger, starving, eating, venison from deer and cooking it.
voxel_test(FoodTest LIBS voxel_server voxel_client GROUPS hunger food)
# Chests: storing, sharing, breaking and saving.
voxel_test(ChestTest LIBS voxel_server voxel_client)
# Beds: respawning at your bed, and sleeping through the night.
voxel_test(BedTest LIBS voxel_server voxel_client)
# Air power: wheels, compressors and what runs on them; factories of machines, miners
# and structures.
voxel_test(PowerTest LIBS voxel_server voxel_client)
voxel_test(FactoryTest LIBS voxel_server voxel_client GROUPS machines mining structures)
# Signals: levers, gates, sensors and what listens to them.
voxel_test(SignalTest LIBS voxel_server voxel_client)
# The data files (Resources/Data), and the item panel's list.
voxel_test(DataTest LIBS voxel_common glfw)
# Two players see the same of structures, chests, fluids, miners and guards.
voxel_test(SyncTest LIBS voxel_server voxel_client)
# Lava and steam: lava running and draining, water boiling off it into steam, steam rising,
# spreading and fading, and the steam reaching a client.
voxel_test(SteamTest LIBS voxel_server voxel_client)
# Fire: spreading through a wood, dying down, put out (water, smothering, buckets),
# burning players and items.
voxel_test(FireTest LIBS voxel_server voxel_client)
