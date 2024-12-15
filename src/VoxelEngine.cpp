#include "VoxelEngine.h"

#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <glm.hpp>

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>

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
SDL_Window* window;
SDL_GLContext context;
TerrainGenerator* generator;
World* world;

int Chunk::chunkCount = 0;

inline static int square(int x)
{
    return x * x;
}

void error_callback(int error, const char *description)
{
    std::cerr << "Error: " << description << std::endl;
}

int init_gl() {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
    {
        std::cerr << "ERROR: Failed to initialize SDL" << std::endl;
        return -1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif

    window = SDL_CreateWindow("VoxelEngine", frameWidth, frameHeight, SDL_WINDOW_OPENGL);
    if (!window)
    {
        std::cerr << "ERROR: Failed to create window" << std::endl;
        return -1;
    }

    context = SDL_GL_CreateContext(window);
    if (!context)
    {
        std::cerr << "ERROR: Failed to create OpenGL context" << std::endl;
        return -1;
    }

#ifndef __EMSCRIPTEN__
    if (!gladLoadGL())
    {
        std::cerr << "ERROR: Failed to initialize GLAD" << std::endl;
        return -1;
    }
#endif

    SDL_GL_SetSwapInterval(1); // vsync

	return 0;
}

bool closeWindow = false;
ThreadPool threadPool;

void VoxelEngine::Shutdown()
{
    threadPool.Stop();

    delete world;
    delete generator;

    closeWindow = true;
    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

void VoxelEngine::Initialize()
{
    if (init_gl())
    {
        closeWindow = true;
    }
}

float lastFrame = 0;

bool main_loop(double time, void *userData)
{
    double currentTime = SDL_GetTicks() / 1000.0f;
    double deltaTime = currentTime - lastFrame;
    lastFrame = currentTime;

    SDL_Event windowEvent;
    while (SDL_PollEvent(&windowEvent))
    {
		debugMenu.ProcessEvent(&windowEvent);

        if (windowEvent.type == SDL_EVENT_QUIT) closeWindow = true;

		if (windowEvent.type == SDL_EVENT_KEY_DOWN)
		{
			if (windowEvent.key.key == SDLK_ESCAPE)
			{
				closeWindow = true;
			}
			
			if (windowEvent.key.key == SDLK_TAB)
			{
				captureCursor = !captureCursor;
			}
			if (windowEvent.key.key == SDLK_F1)
			{
				debugMenu.ToggleWireframe();
			}
			if (windowEvent.key.key == SDLK_F2)
			{
				debugMenu.ToggleDebug();
			}
            if (windowEvent.key.key == SDLK_F3)
            {
                renderer.ReloadShaders();
            }
			if (windowEvent.key.key == SDLK_F4)
			{
				world->RebuildAllChunks();
			}
		}

        if (windowEvent.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        {
            if (windowEvent.button.button == SDL_BUTTON_LEFT)
            {
                glm::ivec3 pos;
                glm::vec3 norm;
                if (world->TraceRay(camera.GetPosition(), camera.GetDirection(), 50, pos, norm))
                {
                    world->SetBlock(pos.x, pos.y, pos.z, BlockType::AIR);
                }
            }
            if (windowEvent.button.button == SDL_BUTTON_RIGHT)
            {
                glm::ivec3 pos;
                glm::vec3 norm;
                if (world->TraceRay(camera.GetPosition(), camera.GetDirection(), 50, pos, norm))
                {
                    Block block;
                    pos += norm;
                    world->SetBlock(pos.x, pos.y, pos.z, BlockType::DIRT);
                }
            }
        }

        if (windowEvent.type == SDL_EVENT_MOUSE_MOTION) {
            if (captureCursor) camera.UpdateLook(windowEvent.motion.xrel, windowEvent.motion.yrel);
        }
    }
    
    const bool* keys = SDL_GetKeyboardState(NULL);

    SDL_SetWindowRelativeMouseMode(window, captureCursor);

    camera.UpdateMove(keys, deltaTime);

    // Laser
    if (keys[SDL_SCANCODE_L])
    {
        glm::ivec3 pos;
        glm::vec3 norm;
        std::vector<glm::ivec3> rayBlocks;
        if (world->TraceRay(camera.GetPosition(), camera.GetDirection(), 100, pos, norm, &rayBlocks))
        {
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
                                world->SetBlock(rayPos.x + x, rayPos.y + y, rayPos.z + z, BlockType::AIR);
                        }
                    }
                }
            }
        }
    }

    world->Update(camera.GetPosition(), camera.GetDirection(), threadPool);

    renderer.Render(*world, camera, debugMenu, deltaTime);

	SDL_GL_SetSwapInterval(debugMenu.GetVSync() ? 1 : 0); // vsync

    SDL_GL_SwapWindow(window);

    return true;
}

void VoxelEngine::Start()
{
    if (closeWindow)
    {
        return;
    }

    threadPool.Start();

    generator = new TerrainGenerator();
    world = new World(generator);

    const char *glsl_version = "#version 330 core";
    debugMenu.Initialize(window, context, glsl_version);
    renderer.Initialize(frameWidth, frameHeight);

#ifdef __EMSCRIPTEN__
    emscripten_request_animation_frame_loop(main_loop, 0);
#else
    while (!closeWindow)
    {
        main_loop(0, nullptr);
    }
#endif
}