cmake_minimum_required(VERSION 3.16.0 FATAL_ERROR)

set(CMAKE_SYSTEM_VERSION 10.0 CACHE STRING "" FORCE)

project(VoxelEngine C CXX)

if(EMSCRIPTEN)
else()
    find_package(glfw3 CONFIG REQUIRED)
    find_package(glad CONFIG REQUIRED)
endif()
find_package(glm CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

find_path(STB_INCLUDE_DIRS "stb_image.h") 

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
                   src/DebugMenu.h)

target_include_directories(${PROJECT_NAME} PRIVATE ${STB_INCLUDE_DIRS} src)



if (EMSCRIPTEN)
    set(CMAKE_EXECUTABLE_SUFFIX ".html")
    set(VCPKG_HOST_TRIPLET wasm32-emscripten)
    set(VCPKG_TARGET_TRIPLET wasm32-emscripten)
    target_link_options(${PROJECT_NAME} PRIVATE -sUSE_GLFW=3 -sMAX_WEBGL_VERSION=2 -sWASM=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE glm::glm imgui::imgui)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE glfw glad::glad glm::glm imgui::imgui)
endif (EMSCRIPTEN)

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