#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <glm/glm.hpp>
//#include <glm/gtx/component_wise.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <imgui.h>

#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
//#include "misc/cpp/imgui_stdlib.h"

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>

#include "Camera.h"
#include "Shader.h"
#include "World.h"
#include "Block.h"

bool captureCursor = true;
bool wireframe = false;
bool debug = false;

float frameWidth = 800.0f, frameHeight = 600.0f;

Camera camera = Camera();
Shader *shader;
GLFWwindow* window;

int Chunk::chunkCount = 0;

inline int square(int x) {
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
        shader->ReloadShader();
    if (key == GLFW_KEY_TAB && action == GLFW_PRESS) {
        captureCursor = !captureCursor;
    }
    if (key == GLFW_KEY_F1 && action == GLFW_PRESS) {
        wireframe = !wireframe;
        glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
        if (wireframe) glDisable(GL_CULL_FACE); else glEnable(GL_CULL_FACE);
    }
    if (key == GLFW_KEY_F2 && action == GLFW_PRESS) {
        debug = !debug;
    }
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos)
{
    if (captureCursor && !debug)
        camera.UpdateLook(xpos, ypos);
    else
        camera.firstMouse = true;
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    glViewport(0, 0, width, height);
    frameWidth = (float)width;
    frameHeight = (float)height;
}

double _update_fps_counter(GLFWwindow* window) {
    static double previous_seconds = glfwGetTime();
    static int frame_count;
    double current_seconds = glfwGetTime();
    double elapsed_seconds = current_seconds - previous_seconds;

    double fps = (double)frame_count / elapsed_seconds;

    if (elapsed_seconds > 0.25) {
        previous_seconds = current_seconds;
        std::string tmp = "VoxelEngine @ fps: " + std::to_string(fps);
        glfwSetWindowTitle(window, tmp.c_str());
        frame_count = 0;
    }
    frame_count++;

    return fps;
}

int init_glfw() {
    glfwSetErrorCallback(error_callback);

    if (!glfwInit())
    {
        std::cerr << "ERROR: Failed to initialize GLFW" << std::endl;
        return -1;
    }

    const char* glsl_version = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    window = glfwCreateWindow(640, 480, "VoxelEngine", NULL, NULL);
    if (!window)
    {
        std::cerr << "ERROR: Failed to create window" << std::endl;
        return -1;
    }

    glfwMakeContextCurrent(window);

    if (!gladLoadGL())
    {
        std::cerr << "ERROR: Failed to initialize GLAD" << std::endl;
        return -1;
    }

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    glfwSetKeyCallback(window, key_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); //(void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    return 0;
}

glm::vec3 ScreenToWorldRay(const Camera& camera, float screenX, float screenY, int screenWidth, int screenHeight) {
    // Convert screen coordinates to normalized device coordinates (NDC)
    float x = (2.0f * screenX) / screenWidth - 1.0f;
    float y = 1.0f - (2.0f * screenY) / screenHeight;
    glm::vec4 rayNDC(x, y, -1.0f, 1.0f); // NDC space is [-1, 1]

    // Clip space coordinates
    glm::vec4 rayClip = rayNDC;

    // Eye coordinates (inverse projection transform)
    glm::vec4 rayEye = glm::inverse(camera.GetProjectionMatrix(screenWidth, screenHeight)) * rayClip;
    rayEye = glm::vec4(rayEye.x, rayEye.y, -1.0f, 0.0f);

    // World coordinates (inverse view transform)
    glm::vec3 rayWorld = glm::vec3(glm::inverse(camera.GetViewMatrix()) * rayEye);
    rayWorld = glm::normalize(rayWorld);

    return rayWorld;
}


// Perform raycasting to find the target block
bool GetTargetBlock(World& world, const glm::vec3& rayOrigin, const glm::vec3& rayDirection, int maxDistance, glm::ivec3& hitBlockPos, Block& hitBlock) {
    float t = 0.0f;
    for (int i = 0; i < maxDistance * 10; ++i) {
        glm::vec3 point = rayOrigin + (t * rayDirection);
        int x = static_cast<int>(std::floor(point.x));
        int y = static_cast<int>(std::floor(point.y));
        int z = static_cast<int>(std::floor(point.z));

        hitBlock = world.GetBlock(x, y, z);
        if (hitBlock.type != BlockType::AIR) {
            hitBlockPos = glm::ivec3(x, y, z);
            return true;
        }

        const float step = 0.1f;
        t += step;
    }
    return false;
}

// Change the target block
void ChangeTargetBlock(World& world, const Camera& camera, BlockType type, int screenWidth, int screenHeight, int maxDistance) {
    glm::vec3 rayDirection = camera.GetDirection();// ScreenToWorldRay(camera, screenWidth / 2.0f, screenHeight / 2.0f, screenWidth, screenHeight);
    glm::vec3 rayOrigin = camera.GetPosition();

    glm::ivec3 pos;
    Block block;
    if (GetTargetBlock(world, rayOrigin, rayDirection, maxDistance, pos, block)) {
        world.SetBlock(pos.x, pos.y, pos.z, type);
    }
}

void ShrinkTargetBlock(World& world, const Camera& camera, int screenWidth, int screenHeight, int maxDistance) {
    glm::vec3 rayDirection = camera.GetDirection();// ScreenToWorldRay(camera, screenWidth / 2.0f, screenHeight / 2.0f, screenWidth, screenHeight);
    glm::vec3 rayOrigin = camera.GetPosition();

    glm::ivec3 pos;
    Block block;
    if (GetTargetBlock(world, rayOrigin, rayDirection, maxDistance, pos, block)) {
        block.Shrink();
        world.SetBlock(pos.x, pos.y, pos.z, block);
    }
}

bool closeWindow = false;

void LoadChunks(World& world) {
    while (!closeWindow) {
        //world.LoadChunks();
        //std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

int main()
{
    if (init_glfw()) {
        return -1;
    }

    Shader s = Shader("Resources/Shaders/main.vert", "Resources/Shaders/main.frag");
    shader = &s;

    int width, height, nrChannels;
    unsigned char* data = stbi_load("Resources/Textures/atlas.png", &width, &height, &nrChannels, 0);

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    if (data)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    else
    {
        std::cerr << "Failed to load texture" << std::endl;
        return -1;
    }

    stbi_image_free(data);

    glEnable(GL_DEPTH_TEST);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW);

    TerrainGenerator generator = TerrainGenerator();

    World world = World(&generator);

    //std::thread chunkLoading(LoadChunks, std::ref(world));

    /*Chunk* chunk = nullptr;
    if (world.GetChunk(chunk, 0, 0, 0)) {
        for (size_t x = 0; x < Chunk::CHUNK_SIZE; x++)
        {
            for (size_t y = 0; y < Chunk::CHUNK_SIZE; y++)
            {
                for (size_t z = 0; z < Chunk::CHUNK_SIZE; z++)
                {
                    if (square(x - 7) + square(y - 7) + square(z - 7) < 16)
                        chunk->SetBlock(x, y, z, BlockType::AIR, false);
                }
            }
        }

        chunk->GenerateMesh(world);
    }*/

    bool vsync = true;
    ImVec4 clear_color = ImVec4(0.26f, 0.47f, 0.71f, 1.0f);

    bool mousePressed = false;
    double lastFrame = 0.0f;
    float frameTimes[100]{};
    int i = 0;

    double frameTimer = 0.0f;

    while (!glfwWindowShouldClose(window))
    {
        double currentFrame = glfwGetTime();
        double deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        float frameTime = 1000.0f * deltaTime;
        float fps = 1.0f / deltaTime;

        frameTimer += deltaTime;

        if (frameTimer > 0.01f) {
            frameTimes[i] = frameTime;
            i++;
            if (i >= 100) i = 0;
            frameTimer = 0.0f;
        }
        
        glfwPollEvents();
        
        glfwSetInputMode(window, GLFW_CURSOR, captureCursor && !debug ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (debug) {
            if (ImGui::Begin("Debug", &debug)) {
                ImGui::Checkbox("VSync", &vsync);
                ImGui::ColorEdit3("Clear Color", (float*)&clear_color);
                auto pos = camera.GetPosition();
                auto angles = camera.GetDirectionAngles();
                ImGui::Text("Position %.3f, %.3f, %.3f", pos.x, pos.y, pos.z);
                ImGui::Text("Angles %.3f, %.3f", angles.x, angles.y);
                ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", frameTime, fps);
                ImGui::PlotLines("Frame Time (ms)", frameTimes, 100, i);
                ImGui::Text("Chunk count: %d", Chunk::chunkCount);
            }

            ImGui::End();
        }
        

        ImGui::Render();

        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (!debug) {
            camera.UpdateMove(window, deltaTime);

            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_1) == GLFW_PRESS && !mousePressed) {
                //ChangeTargetBlock(world, camera, BlockType::AIR, frameWidth, frameHeight, 10);
                ShrinkTargetBlock(world, camera, frameWidth, frameHeight, 10);
                mousePressed = true;
            }
            else if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_1) == GLFW_RELEASE) {
                mousePressed = false;
            }
        }

        world.Update(camera.GetPosition(), camera.GetDirection());

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        
        glm::mat4 view = camera.GetViewMatrix();
        glm::mat4 projection = camera.GetProjectionMatrix(frameWidth, frameHeight);

        world.Render(*shader, view, projection, frameWidth, frameHeight);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapInterval(vsync); // vsync

        glfwSwapBuffers(window);
    }

    closeWindow = true;
    //chunkLoading.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}