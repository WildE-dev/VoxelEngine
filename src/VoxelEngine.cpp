#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <glad/glad.h>
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>

#include "VoxelEngine.h"

#include "Camera.h"
#include "Shader.h"
#include "World.h"
#include "Block.h"
#include "ThreadPool.h"
#include "Renderer.h"
#include "DebugMenu.h"

bool captureCursor = true;


#ifdef __APPLE__
const float frameWidth = 1600.0f, frameHeight = 1200.0f;
#else
const float frameWidth = 800.0f, frameHeight = 600.0f;
#endif
Camera camera;
DebugMenu debugMenu;
Renderer renderer;
GLFWwindow* window;

int Chunk::chunkCount = 0;

inline static int square(int x) {
    return x * x;
}

void error_callback(int error, const char* description)
{
    std::cerr << "Error: " << description << std::endl;
}

static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    if (key == GLFW_KEY_R && action == GLFW_PRESS)
        renderer.ReloadShaders();
    if (key == GLFW_KEY_TAB && action == GLFW_PRESS) {
        captureCursor = !captureCursor;
    }
    if (key == GLFW_KEY_F1 && action == GLFW_PRESS) {
        debugMenu.ToggleWireframe();
    }
    if (key == GLFW_KEY_F2 && action == GLFW_PRESS) {
        debugMenu.ToggleDebug();
    }
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos)
{
    if (captureCursor)
        camera.UpdateLook(xpos, ypos);
    else
        camera.firstMouse = true;
}  

int init_glfw() {
    glfwSetErrorCallback(error_callback);

    if (!glfwInit())
    {
        std::cerr << "ERROR: Failed to initialize GLFW" << std::endl;
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
#ifdef __APPLE__
    window = glfwCreateWindow(frameWidth / 2, frameHeight / 2, "VoxelEngine", NULL, NULL);
#else
    window = glfwCreateWindow(frameWidth, frameHeight, "VoxelEngine", NULL, NULL);
#endif
    
    if (!window)
    {
        std::cerr << "ERROR: Failed to create window" << std::endl;
        return -1;
    }

    glfwMakeContextCurrent(window);

#ifndef __EMSCRIPTEN__
    if (!gladLoadGL())
    {
        std::cerr << "ERROR: Failed to initialize GLAD" << std::endl;
        return -1;
    }
#endif

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    glfwSetKeyCallback(window, key_callback);
    glfwSetCursorPosCallback(window, mouse_callback);

    return 0;
}

bool closeWindow = false;
ThreadPool threadPool;

void VoxelEngine::Shutdown() {
    threadPool.Stop();

    closeWindow = true;
    glfwSetErrorCallback(NULL);
    glfwDestroyWindow(window);
    glfwTerminate();
}

void VoxelEngine::Initialize() {
    if (init_glfw()) {
        closeWindow = true;
    }
}

void VoxelEngine::Start()
{
    if (closeWindow) {
        return;
    }

    threadPool.Start();

    TerrainGenerator generator;

    World world = World(&generator);

    const char* glsl_version = "#version 330 core";
    debugMenu.Initialize(window, glsl_version);
    renderer.Initialize(frameWidth, frameHeight);

    std::map<int, bool> buttonsPressed;
    float lastFrame = 0;

    while (!glfwWindowShouldClose(window))
    {
        double deltaTime = glfwGetTime() - lastFrame;
        lastFrame = glfwGetTime();
        
        glfwPollEvents();
        
        glfwSetInputMode(window, GLFW_CURSOR, captureCursor ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);

        camera.UpdateMove(window, deltaTime);

        // Laser
        if (glfwGetKey(window, GLFW_KEY_L) == GLFW_PRESS) {
            glm::ivec3 pos;
            glm::vec3 norm;
            std::vector<glm::ivec3> rayBlocks;
            if (world.TraceRay(camera.GetPosition(), camera.GetDirection(), 100, pos, norm, &rayBlocks)) {
                const int radius = 4;
                for (int i = 0; i < rayBlocks.size(); i++)
                {
                    auto rayPos = rayBlocks[i];
                    for (int x = -radius; x <= radius; x++)
                    {
                        for (int y = -radius; y <= radius; y++)
                        {
                            for (int z = -radius; z <= radius; z++)
                            {
                                if (square(x) + square(y) + square(z) <= square(radius))
                                    world.SetBlock(rayPos.x + x, rayPos.y + y, rayPos.z + z, BlockType::AIR);
                            }
                        }
                    }
                }
            }
        }

        if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_1) == GLFW_PRESS && !buttonsPressed[GLFW_MOUSE_BUTTON_1]) {
            glm::ivec3 pos;
            glm::vec3 norm;
            if (world.TraceRay(camera.GetPosition(), camera.GetDirection(), 50, pos, norm)) {
                world.SetBlock(pos.x, pos.y, pos.z, BlockType::AIR);
            }

            buttonsPressed[GLFW_MOUSE_BUTTON_1] = true;
        }
        else if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_1) == GLFW_RELEASE) {
            buttonsPressed[GLFW_MOUSE_BUTTON_1] = false;
        }

        if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_2) == GLFW_PRESS && !buttonsPressed[GLFW_MOUSE_BUTTON_2]) {
            glm::ivec3 pos;
            glm::vec3 norm;
            if (world.TraceRay(camera.GetPosition(), camera.GetDirection(), 50, pos, norm)) {
                Block block;
                pos += norm;
                world.SetBlock(pos.x, pos.y, pos.z, BlockType::DIRT);
            }

            buttonsPressed[GLFW_MOUSE_BUTTON_2] = true;
        }
        else if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_2) == GLFW_RELEASE) {
            buttonsPressed[GLFW_MOUSE_BUTTON_2] = false;
        }

        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) {
            world.RebuildAllChunks();
        }

        world.Update(camera.GetPosition(), camera.GetDirection(), threadPool);
        
        renderer.Render(world, camera, debugMenu, deltaTime);

        glfwSwapInterval(debugMenu.GetVSync() ? 1 : 0); // vsync

        glfwSwapBuffers(window);
    }
}