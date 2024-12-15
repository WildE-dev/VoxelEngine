cmake_minimum_required(VERSION 3.20)
project(VoxelEngine C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find SDL3
if(WIN32)
    # On Windows, point CMake to the SDL3 CMake files if they are not installed globally
    set(CMAKE_PREFIX_PATH ${CMAKE_SOURCE_DIR}/libs/SDL3/windows/cmake)
elseif(APPLE)
    # On macOS, SDL3 might already be installed via Homebrew or manually
    # If not installed globally, use the local path
    set(CMAKE_PREFIX_PATH ${CMAKE_SOURCE_DIR}/libs/SDL3/macos/cmake)
endif()

find_package(SDL3 REQUIRED CONFIG)

add_executable(${PROJECT_NAME} src/main.cpp
                   src/VoxelEngine.cpp
                   src/Block.cpp
                   src/Camera.cpp
                   src/Chunk.cpp
                   src/Shader.cpp
                   src/TerrainGenerator.cpp
                   src/AsyncCircularQueue.cpp
                   src/World.cpp
                   src/ThreadPool.cpp
                   src/Debugging.cpp
                   src/AssetLoader.cpp
                   src/Renderer.cpp
                   src/DebugMenu.cpp
                   
                   src/VoxelEngine.h
                   src/Common.h
                   src/Block.h
                   src/Camera.h
                   src/Chunk.h
                   src/Shader.h
                   src/TerrainGenerator.h
                   src/World.h
                   src/AsyncCircularQueue.h
                   src/ThreadPool.h
                   src/Debugging.h
                   src/AssetLoader.h
                   src/Renderer.h
                   src/DebugMenu.h

                   libs/glad/src/glad.c)

set(IMGUI_SRC
    libs/imgui/imgui.cpp
    libs/imgui/imgui_draw.cpp
    libs/imgui/imgui_tables.cpp
    libs/imgui/imgui_widgets.cpp
    libs/imgui/backends/imgui_impl_sdl3.cpp
    libs/imgui/backends/imgui_impl_opengl3.cpp
)

target_sources(${PROJECT_NAME} PRIVATE ${IMGUI_SRC})

target_include_directories(${PROJECT_NAME} PRIVATE libs/glm libs/imgui libs/imgui/backends libs/stb_image libs/glad/include)
target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3)

# Creates C resources file from files in given directory
function(create_resources dir output)
    # Create empty output file
    file(WRITE ${output} "")
    # Collect input files
    file(GLOB bins ${dir}/*)
    # Iterate through input files
    foreach(bin ${bins})
        # Get short filename
        string(REGEX MATCH "([^/]+)$" filename ${bin})
        # Replace filename spaces & extension separator for C compatibility
        string(REGEX REPLACE "\\.| |-" "_" filename ${filename})
        # Read hex data from file
        file(READ ${bin} filedata HEX)
        # Convert hex data for C compatibility
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," filedata ${filedata})
        # Append data to output file
        file(APPEND ${output} "const unsigned char ${filename}[] = {${filedata}};\nconst unsigned ${filename}_size = sizeof(${filename});\n")
    endforeach()
endfunction()

# create_resources("Resources/Shaders" "src/ShaderResources.h")
# create_resources("Resources/Textures" "src/TextureResources.h")