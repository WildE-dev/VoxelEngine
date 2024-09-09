cmake_minimum_required(VERSION 3.16.0 FATAL_ERROR)

set(CMAKE_SYSTEM_VERSION 10.0 CACHE STRING "" FORCE)

project(VoxelEngine C CXX)

find_package(glfw3 REQUIRED)
find_package(glad REQUIRED)
find_package(glm REQUIRED)
find_package(imgui REQUIRED)

find_path(STB_INCLUDE_DIRS "stb_image.h") 

add_executable(${PROJECT_NAME} src/VoxelEngine.cpp
                   src/Block.cpp
                   src/Camera.cpp
                   src/Chunk.cpp
                   src/Shader.cpp
                   src/TerrainGenerator.cpp
                   src/AsyncCircularQueue.cpp
                   src/World.cpp
                   src/Block.h
                   src/Camera.h
                   src/Chunk.h
                   src/Shader.h
                   src/TerrainGenerator.h
                   src/World.h
                   src/AsyncCircularQueue.h)

target_include_directories(${PROJECT_NAME} PRIVATE ${STB_INCLUDE_DIRS} src)

target_link_libraries(${PROJECT_NAME} PRIVATE glfw glad::glad glm::glm imgui::imgui)