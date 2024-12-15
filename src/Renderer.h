#pragma once

#include <glm.hpp>

#include <glad/glad.h>

#include "World.h"
#include "Camera.h"
#include "Shader.h"
#include "DebugMenu.h"

class Renderer {
public:
    Renderer();
    ~Renderer();

    void Initialize(int frameWidth, int frameHeight);
    void DrawCube(glm::mat4 view, glm::mat4 projection, glm::vec3 pos);
    void Render(World& world, Camera& camera, DebugMenu& debugMenu, double deltaTime);
    void Cleanup();
    void ReloadShaders();

private:
    GLuint framebuffer, textureColorbuffer, rbo;
    GLuint skyboxVAO, skyboxVBO;
    GLuint screenVAO, screenVBO;
    GLuint holeVAO, holeVBO;
    GLuint cubeVAO, cubeVBO;
    GLuint cubemapTexture, atlasTexture;
    Shader* shaders[5];

    void SetupCube();
    void SetupSkybox();
    void SetupFramebuffer(int frameWidth, int frameHeight);
    void SetupScreenQuad();
    void SetupHole();

    void RenderWorld(World& world, Camera& camera, glm::mat4 view, glm::mat4 projection);
    void RenderSkybox(glm::mat4 view, glm::mat4 projection);
    void RenderHole(glm::mat4 view, glm::mat4 projection);
    void RenderScreen();

    glm::vec2 resolution;
};
