#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <glm/glm.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <imgui.h>

#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

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
#include "Debugging.h"

bool captureCursor = true;
bool wireframe = false;
bool debug = false;

float frameWidth = 1600.0f, frameHeight = 1200.0f;

Camera camera = Camera();
Shader *shaders[5];
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
        for (size_t i = 0; i < sizeof(shaders) / sizeof(Shader*); i++)
        {
            shaders[i]->ReloadShader();
        }
    if (key == GLFW_KEY_TAB && action == GLFW_PRESS) {
        captureCursor = !captureCursor;
    }
    if (key == GLFW_KEY_F1 && action == GLFW_PRESS) {
        wireframe = !wireframe;
    }
    if (key == GLFW_KEY_F2 && action == GLFW_PRESS) {
        debug = !debug;
    }
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos)
{
    if (captureCursor)
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

GLuint loadCubemap(std::array<std::string, 6> faces)
{
    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_CUBE_MAP, textureID);

    int width, height, nrChannels;
    for (unsigned int i = 0; i < faces.size(); i++)
    {
        unsigned char *data = stbi_load(faces[i].c_str(), &width, &height, &nrChannels, 0);
        if (data)
        {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 
                         0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data
            );
        }
        else
        {
            std::cout << "Cubemap tex failed to load at path: " << faces[i] << std::endl;
        }
        stbi_image_free(data);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    return textureID;
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

    const char* glsl_version = "#version 330 core";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    window = glfwCreateWindow(frameWidth / 2, frameHeight / 2, "VoxelEngine", NULL, NULL);
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
    ImGuiIO& io = ImGui::GetIO();
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

static bool TraceRay(World& world, glm::vec3 p, glm::vec3 dir, float max_d, glm::ivec3& hit_pos, glm::vec3& hit_norm, std::vector<glm::ivec3>* rayBlocks = nullptr) {

    // consider raycast vector to be parametrized by t
    //   vec = [px,py,pz] + t * [dx,dy,dz]

    // algo below is as described by this paper:
    // http://www.cse.chalmers.se/edu/year/2010/course/TDA361/grid.pdf

    float t = 0.0;
    int ix = std::floor(p.x);
    int iy = std::floor(p.y);
    int iz = std::floor(p.z);
    
    int stepx = (dir.x > 0) ? 1 : -1;
    int stepy = (dir.y > 0) ? 1 : -1;
    int stepz = (dir.z > 0) ? 1 : -1;
    
    // dir is already normalized
    float txDelta = std::abs(1.0f / dir.x);
    float tyDelta = std::abs(1.0f / dir.y);
    float tzDelta = std::abs(1.0f / dir.z);

    float xdist = (stepx > 0) ? (ix + 1 - p.x) : (p.x - ix);
    float ydist = (stepy > 0) ? (iy + 1 - p.y) : (p.y - iy);
    float zdist = (stepz > 0) ? (iz + 1 - p.z) : (p.z - iz);

    // location of nearest voxel boundary, in units of t 
    float txMax = (txDelta < FLT_MAX) ? txDelta * xdist : FLT_MAX;
    float tyMax = (tyDelta < FLT_MAX) ? tyDelta * ydist : FLT_MAX;
    float tzMax = (tzDelta < FLT_MAX) ? tzDelta * zdist : FLT_MAX;

    int steppedIndex = -1;

    if (rayBlocks)
        rayBlocks->clear();

    bool b = false;

    // main loop along raycast vector
    while (t <= max_d) {
        Block hitBlock;
        if (world.GetBlock(ix, iy, iz, hitBlock)) {
            if (hitBlock.type != BlockType::AIR) {
                b = true;
            }

            if (rayBlocks)
                rayBlocks->emplace_back(ix, iy, iz);
        }

        // exit check
        if (b && !rayBlocks) {
            hit_pos = { ix, iy, iz };
            hit_norm = { 0, 0, 0 };
            if (steppedIndex == 0) hit_norm.x = -stepx;
            if (steppedIndex == 1) hit_norm.y = -stepy;
            if (steppedIndex == 2) hit_norm.z = -stepz;
            return b;
        }

        // advance t to next nearest voxel boundary
        if (txMax < tyMax) {
            if (txMax < tzMax) {
                ix += stepx;
                t = txMax;
                txMax += txDelta;
                steppedIndex = 0;
            }
            else {
                iz += stepz;
                t = tzMax;
                tzMax += tzDelta;
                steppedIndex = 2;
            }
        }
        else {
            if (tyMax < tzMax) {
                iy += stepy;
                t = tyMax;
                tyMax += tyDelta;
                steppedIndex = 1;
            }
            else {
                iz += stepz;
                t = tzMax;
                tzMax += tzDelta;
                steppedIndex = 2;
            }
        }

    }

    // no voxel hit found
    hit_pos = p + t * dir;
    hit_norm = { 0, 0, 0 };

    return b;
}

bool closeWindow = false;

int main()
{
    if (init_glfw()) {
        return -1;
    }

    Shader s = Shader("Resources/Shaders/main.vert", "Resources/Shaders/main.frag");
    Shader debugShader = Shader("Resources/Shaders/debug.vert", "Resources/Shaders/debug.frag");
    Shader skyboxShader = Shader("Resources/Shaders/skybox.vert", "Resources/Shaders/skybox.frag");
    Shader screenShader = Shader("Resources/Shaders/screen.vert", "Resources/Shaders/screen.frag");
    Shader holeShader = Shader("Resources/Shaders/hole.vert", "Resources/Shaders/hole.frag");
    shaders[0] = &s;
    shaders[1] = &debugShader;
    shaders[2] = &skyboxShader;
    shaders[3] = &screenShader;
    shaders[4] = &holeShader;

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

    std::array<std::string, 6> faces
    {
        "Resources/Textures/cubemap/sh_ft.png",
        "Resources/Textures/cubemap/sh_bk.png",
        "Resources/Textures/cubemap/sh_up.png",
        "Resources/Textures/cubemap/sh_dn.png",
        "Resources/Textures/cubemap/sh_rt.png",
        "Resources/Textures/cubemap/sh_lf.png",
    };
    GLuint cubemapTexture = loadCubemap(faces);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW);

    ThreadPool threadPool;
    threadPool.Start();

    TerrainGenerator generator;

    World world = World(&generator);

    Debugging debugging = Debugging();

    #pragma region SKYBOX SETUP

    GLuint skyboxVAO, skyboxVBO;

    glGenVertexArrays(1, &skyboxVAO);
    glGenBuffers(1, &skyboxVBO);

    glBindVertexArray(skyboxVAO);

    float skyboxVerts[] = {
        -0.5,-0.5,-0.5,
        0.5,0.5,-0.5,
        0.5,-0.5,-0.5,

        -0.5,-0.5,-0.5,
        -0.5,0.5,-0.5,
        0.5,0.5,-0.5,

        -0.5,0.5,-0.5,
        0.5,0.5,0.5,
        0.5,0.5,-0.5,

        -0.5,0.5,-0.5,
        -0.5,0.5,0.5,
        0.5,0.5,0.5,

        -0.5,-0.5,-0.5,
        -0.5,-0.5,0.5,
        -0.5,0.5,0.5,

        -0.5,-0.5,-0.5,
        -0.5,0.5,0.5,
        -0.5,0.5,-0.5,

        -0.5,-0.5,-0.5,
        0.5,-0.5,-0.5,
        0.5,-0.5,0.5,

        -0.5,-0.5,-0.5,
        0.5,-0.5,0.5,
        -0.5,-0.5,0.5,

        -0.5,-0.5,0.5,
        0.5,-0.5,0.5,
        0.5,0.5,0.5,

        -0.5,-0.5,0.5,
        0.5,0.5,0.5,
        -0.5,0.5,0.5,

        0.5,-0.5,-0.5,
        0.5,0.5,0.5,
        0.5,-0.5,0.5,

        0.5,-0.5,-0.5,
        0.5,0.5,-0.5,
        0.5,0.5,0.5,
    };
    
    glBindBuffer(GL_ARRAY_BUFFER, skyboxVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVerts), skyboxVerts, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);

    #pragma endregion

    #pragma region FRAMEBUFFER

    GLuint screenVAO, screenVBO;

    glGenVertexArrays(1, &screenVAO);
    glGenBuffers(1, &screenVBO);

    glBindVertexArray(screenVAO);

    float screenVerts[] = {
        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,

        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
    };
    
    glBindBuffer(GL_ARRAY_BUFFER, screenVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(screenVerts), screenVerts, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    GLuint framebuffer;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    // create a color attachment texture

    GLuint textureColorbuffer;
    glGenTextures(1, &textureColorbuffer);
    glBindTexture(GL_TEXTURE_2D, textureColorbuffer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frameWidth, frameHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureColorbuffer, 0);

    // create a renderbuffer object for depth and stencil attachment (we won't be sampling these)
    GLuint rbo;
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, frameWidth, frameHeight); // use a single renderbuffer object for both a depth AND stencil buffer.
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo); // now actually attach it
    // now that we actually created the framebuffer and added all attachments we want to check if it is actually complete now
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cout << "ERROR::FRAMEBUFFER:: Framebuffer is not complete!" << std::endl;
        return -1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    #pragma endregion

    GLuint holeVAO, holeVBO;

    glGenVertexArrays(1, &holeVAO);
    glGenBuffers(1, &holeVBO);

    glBindVertexArray(holeVAO);

    float holeVerts[] = {
        -1.0f,  1.0f, 0.0f,  0.0f, 1.0f,
         1.0f, -1.0f, 0.0f,  1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f,  0.0f, 0.0f,

        -1.0f,  1.0f, 0.0f,  0.0f, 1.0f,
         1.0f,  1.0f, 0.0f,  1.0f, 1.0f,
         1.0f, -1.0f, 0.0f,  1.0f, 0.0f,
    };
    
    glBindBuffer(GL_ARRAY_BUFFER, holeVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(holeVerts), holeVerts, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    bool vsync = true;

    //bool mousePressed = false;
    std::map<int, bool> buttonsPressed;
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
        
        glfwSetInputMode(window, GLFW_CURSOR, captureCursor ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);

        camera.UpdateMove(window, deltaTime);

        // Laser
        if (glfwGetKey(window, GLFW_KEY_L) == GLFW_PRESS) {
            glm::ivec3 pos;
            glm::vec3 norm;
            std::vector<glm::ivec3> rayBlocks;
            if (TraceRay(world, camera.GetPosition(), camera.GetDirection(), 100, pos, norm, &rayBlocks)) {
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
            if (TraceRay(world, camera.GetPosition(), camera.GetDirection(), 50, pos, norm)) {
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
            if (TraceRay(world, camera.GetPosition(), camera.GetDirection(), 50, pos, norm)) {
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
        
        glm::mat4 view = camera.GetViewMatrix();
        glm::mat4 projection = camera.GetProjectionMatrix(frameWidth, frameHeight);

        glBindFramebuffer(GL_FRAMEBUFFER, rbo);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glEnable(GL_DEPTH_TEST);

        glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
        if (wireframe) glDisable(GL_CULL_FACE); else glEnable(GL_CULL_FACE);

        glViewport(0, 0, frameWidth, frameHeight);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        world.Render(s, view, projection, frameWidth, frameHeight, currentFrame);

        glEnable(GL_CULL_FACE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        glm::mat4 skyboxView = glm::mat4(glm::mat3(view));

        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);

        skyboxShader.Use();
        skyboxShader.SetUniform("view", skyboxView);
        skyboxShader.SetUniform("projection", projection);

        glBindVertexArray(skyboxVAO);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cubemapTexture);
        glDrawArrays(GL_TRIANGLES, 0, 36);

        holeShader.Use();
        glm::mat4 model = glm::mat4(1);
        model = glm::translate(model, glm::vec3(0, 2, 0));
        holeShader.SetUniform("model", model);
        holeShader.SetUniform("view", view);
        holeShader.SetUniform("projection", projection);
        holeShader.SetUniform("time", currentFrame);
        holeShader.SetUniform("uResolution", glm::vec2(frameWidth, frameHeight));

        glBindVertexArray(holeVAO);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textureColorbuffer);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);

        glm::ivec3 pos;
        glm::vec3 norm;
        if (TraceRay(world, camera.GetPosition(), camera.GetDirection(), 50, pos, norm)) {
            debugging.DrawCube(debugShader, view, projection, pos);
        }

        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_DEPTH_TEST);

        screenShader.Use();
        glBindVertexArray(screenVAO);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textureColorbuffer);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (debug) {
            if (ImGui::Begin("Debug", &debug)) {
                ImGui::Checkbox("VSync", &vsync);
                auto pos = camera.GetPosition();
                auto angles = camera.GetDirectionAngles();
                ImGui::Text("Position %.3f, %.3f, %.3f", pos.x, pos.y, pos.z);
                ImGui::Text("Angles %.3f, %.3f", angles.x, angles.y);
                ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", frameTime, fps);
                ImGui::PlotLines("Frame Time (ms)", frameTimes, 100, i);
                ImGui::Text("Chunk count: %d", Chunk::chunkCount);
                ImGui::Image((void*)(intptr_t)textureColorbuffer, ImVec2(frameWidth, frameHeight));
            }

            ImGui::End();
        }
        
        ImGui::Render();

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapInterval(vsync); // vsync

        glfwSwapBuffers(window);
    }

    threadPool.Stop();

    closeWindow = true;
    
    glDeleteFramebuffers(1, &rbo);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}