#include "render/Renderer.h"

#include <iostream>

#include <glm/gtc/matrix_transform.hpp>

#include "render/Textures.h"
#include "world/World.h"

namespace {

const float kSkyboxVerts[] = {
    -0.5f,-0.5f,-0.5f,   0.5f, 0.5f,-0.5f,   0.5f,-0.5f,-0.5f,
    -0.5f,-0.5f,-0.5f,  -0.5f, 0.5f,-0.5f,   0.5f, 0.5f,-0.5f,

    -0.5f, 0.5f,-0.5f,   0.5f, 0.5f, 0.5f,   0.5f, 0.5f,-0.5f,
    -0.5f, 0.5f,-0.5f,  -0.5f, 0.5f, 0.5f,   0.5f, 0.5f, 0.5f,

    -0.5f,-0.5f,-0.5f,  -0.5f,-0.5f, 0.5f,  -0.5f, 0.5f, 0.5f,
    -0.5f,-0.5f,-0.5f,  -0.5f, 0.5f, 0.5f,  -0.5f, 0.5f,-0.5f,

    -0.5f,-0.5f,-0.5f,   0.5f,-0.5f,-0.5f,   0.5f,-0.5f, 0.5f,
    -0.5f,-0.5f,-0.5f,   0.5f,-0.5f, 0.5f,  -0.5f,-0.5f, 0.5f,

    -0.5f,-0.5f, 0.5f,   0.5f,-0.5f, 0.5f,   0.5f, 0.5f, 0.5f,
    -0.5f,-0.5f, 0.5f,   0.5f, 0.5f, 0.5f,  -0.5f, 0.5f, 0.5f,

     0.5f,-0.5f,-0.5f,   0.5f, 0.5f, 0.5f,   0.5f,-0.5f, 0.5f,
     0.5f,-0.5f,-0.5f,   0.5f, 0.5f,-0.5f,   0.5f, 0.5f, 0.5f,
};

// Fullscreen quad: position (xy), uv.
const float kScreenVerts[] = {
    -1.0f,  1.0f,  0.0f, 1.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
    -1.0f, -1.0f,  0.0f, 0.0f,

    -1.0f,  1.0f,  0.0f, 1.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
};

// Camera-facing billboard for the black hole: position (xyz), uv.
const float kHoleVerts[] = {
    -5.0f,  5.0f, 0.0f,  0.0f, 1.0f,
     5.0f, -5.0f, 0.0f,  1.0f, 0.0f,
    -5.0f, -5.0f, 0.0f,  0.0f, 0.0f,

    -5.0f,  5.0f, 0.0f,  0.0f, 1.0f,
     5.0f,  5.0f, 0.0f,  1.0f, 1.0f,
     5.0f, -5.0f, 0.0f,  1.0f, 0.0f,
};

const glm::vec3 kBlackHolePosition(0.0f, 2.0f, 0.0f);
const char* kBlockTextureDirectory = "Resources/Textures/Blocks";
const glm::vec3 kBlockLightColor(1.0f, 0.85f, 0.6f); // warm lamp light

void CreateVertexBuffer(GLuint& vao, GLuint& vbo, const float* data, GLsizeiptr size) {
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, size, data, GL_STATIC_DRAW);
}

} // namespace

Renderer::Renderer()
    : worldShader("Resources/Shaders/main.vert", "Resources/Shaders/main.frag"),
      debugShader("Resources/Shaders/debug.vert", "Resources/Shaders/debug.frag"),
      skyboxShader("Resources/Shaders/skybox.vert", "Resources/Shaders/skybox.frag"),
      screenShader("Resources/Shaders/screen.vert", "Resources/Shaders/screen.frag"),
      holeShader("Resources/Shaders/hole.vert", "Resources/Shaders/hole.frag") {
}

Renderer::~Renderer() {
    DestroySceneFramebuffer();

    glDeleteVertexArrays(1, &skyboxVAO);
    glDeleteBuffers(1, &skyboxVBO);
    glDeleteVertexArrays(1, &screenVAO);
    glDeleteBuffers(1, &screenVBO);
    glDeleteVertexArrays(1, &holeVAO);
    glDeleteBuffers(1, &holeVBO);

    glDeleteTextures(1, &skyboxTexture);
}

bool Renderer::Init(int framebufferWidth, int framebufferHeight) {
    width = framebufferWidth;
    height = framebufferHeight;

    atlas.Build(kBlockTextureDirectory);
    skyboxTexture = Textures::LoadCubemap({
        "Resources/Textures/right.bmp",
        "Resources/Textures/left.bmp",
        "Resources/Textures/top.bmp",
        "Resources/Textures/bottom.bmp",
        "Resources/Textures/front.bmp",
        "Resources/Textures/back.bmp",
    });

    CreateGeometry();

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW);

    return CreateSceneFramebuffer();
}

void Renderer::CreateGeometry() {
    CreateVertexBuffer(skyboxVAO, skyboxVBO, kSkyboxVerts, sizeof(kSkyboxVerts));
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    CreateVertexBuffer(screenVAO, screenVBO, kScreenVerts, sizeof(kScreenVerts));
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    CreateVertexBuffer(holeVAO, holeVBO, kHoleVerts, sizeof(kHoleVerts));
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

bool Renderer::CreateSceneFramebuffer() {
    glGenFramebuffers(1, &sceneFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO);

    glGenTextures(1, &sceneColor);
    glBindTexture(GL_TEXTURE_2D, sceneColor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneColor, 0);

    glGenRenderbuffers(1, &sceneDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, sceneDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, sceneDepth);

    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (!complete) {
        std::cerr << "ERROR: Scene framebuffer is not complete" << std::endl;
        return false;
    }
    return true;
}

void Renderer::DestroySceneFramebuffer() {
    glDeleteFramebuffers(1, &sceneFBO);
    glDeleteTextures(1, &sceneColor);
    glDeleteRenderbuffers(1, &sceneDepth);
    sceneFBO = sceneColor = sceneDepth = 0;
}

void Renderer::ReloadShaders() {
    worldShader.Reload();
    debugShader.Reload();
    skyboxShader.Reload();
    screenShader.Reload();
    holeShader.Reload();
}

void Renderer::ReloadTextures() {
    atlas.Build(kBlockTextureDirectory);
}

void Renderer::RenderFrame(World& world, const FrameParams& params) {
    // --- Scene pass into the offscreen framebuffer ---
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO);
    glViewport(0, 0, width, height);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glPolygonMode(GL_FRONT_AND_BACK, params.wireframe ? GL_LINE : GL_FILL);
    if (params.wireframe) glDisable(GL_CULL_FACE); else glEnable(GL_CULL_FACE);

    atlas.Bind(GL_TEXTURE0);
    worldShader.Use();
    worldShader.SetUniform("ourTexture", 0);
    atlas.ApplyUniforms(worldShader);
    worldShader.SetUniform("aoStrength", params.ambientOcclusion ? 1.0f : 0.0f);
    worldShader.SetUniform("sunBrightness", params.sunBrightness);
    worldShader.SetUniform("blockLightColor", kBlockLightColor);
    world.Render(worldShader, params.view, params.projection, params.time);

    glEnable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    DrawSkybox(params);

    if (params.highlightedBlock) {
        debugDraw.DrawWireCube(debugShader, params.view, params.projection, glm::vec3(*params.highlightedBlock));
    }

    // --- Composite to the default framebuffer ---
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);

    screenShader.Use();
    screenShader.SetUniform("screenTexture", 0);
    glBindVertexArray(screenVAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneColor);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    DrawBlackHole(params);

    glBindVertexArray(0);
}

void Renderer::DrawSkybox(const FrameParams& params) {
    // Drawn after the world at max depth so it only fills uncovered pixels.
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);

    skyboxShader.Use();
    skyboxShader.SetUniform("view", glm::mat4(glm::mat3(params.view))); // strip translation
    skyboxShader.SetUniform("projection", params.projection);
    skyboxShader.SetUniform("skybox", 0);
    skyboxShader.SetUniform("brightness", 0.05f + 0.95f * params.sunBrightness);

    glBindVertexArray(skyboxVAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxTexture);
    glDrawArrays(GL_TRIANGLES, 0, 36);

    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
}

void Renderer::DrawBlackHole(const FrameParams& params) {
    holeShader.Use();
    holeShader.SetUniform("model", glm::translate(glm::mat4(1.0f), kBlackHolePosition));
    holeShader.SetUniform("view", params.view);
    holeShader.SetUniform("projection", params.projection);
    holeShader.SetUniform("time", params.time);
    holeShader.SetUniform("uResolution", glm::vec2(width, height));
    holeShader.SetUniform("screenTexture", 0);

    holeShader.SetUniform("blackHoleCenter", glm::vec2(0.5f, 0.5f));
    holeShader.SetUniform("eventHorizon", 0.25f);
    holeShader.SetUniform("lensingRadius", 0.4f);
    holeShader.SetUniform("maxDistortion", 0.3f);

    glBindVertexArray(holeVAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneColor);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}
