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

add_executable(${PROJECT_NAME}
    src/main.cpp

    src/app/Application.cpp
    src/app/Application.h
    src/app/Camera.cpp
    src/app/Camera.h
    src/app/Player.cpp
    src/app/Player.h

    src/net/Protocol.cpp
    src/net/Protocol.h
    src/net/Transport.cpp
    src/net/Transport.h

    src/render/DebugDraw.cpp
    src/render/DebugDraw.h
    src/render/Renderer.cpp
    src/render/Renderer.h
    src/render/Shader.cpp
    src/render/Shader.h
    src/render/Textures.cpp
    src/render/Textures.h
    src/render/TextureAtlas.cpp
    src/render/TextureAtlas.h

    src/world/Block.cpp
    src/world/Block.h
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
    src/world/Water.cpp
    src/world/Water.h
    src/world/World.cpp
    src/world/World.h
)

target_include_directories(${PROJECT_NAME} PRIVATE src ${STB_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE glfw glad::glad glm::glm imgui::imgui unofficial::enet::enet Threads::Threads)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W3)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()

# Copy shaders/textures next to the executable on every build so edits are picked up.
# The app loads them relative to the working directory.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/Resources $<TARGET_FILE_DIR:${PROJECT_NAME}>/Resources
    COMMENT "Copying resources")

set_target_properties(${PROJECT_NAME} PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${PROJECT_NAME}>)
