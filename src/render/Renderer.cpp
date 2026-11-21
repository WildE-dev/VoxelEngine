#include "render/Renderer.h"

#include <iostream>

#include <glm/gtc/matrix_transform.hpp>

#include "render/Textures.h"
#include "client/ClientWorld.h"

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
// The lensing black hole at spawn: off. Set to true to bring it back.
constexpr bool kDrawBlackHole = false;
const char* kBlockTextureDirectory = "Resources/Textures/Blocks";
const glm::vec3 kBlockLightColor(1.0f, 0.85f, 0.6f); // warm lamp light
// Where the sun glints off water from (it isn't tied to the skybox texture).

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
      holeShader("Resources/Shaders/hole.vert", "Resources/Shaders/hole.frag"),
      waterShader("Resources/Shaders/water.vert", "Resources/Shaders/water.frag"),
      rainShader("Resources/Shaders/rain.vert", "Resources/Shaders/rain.frag"),
      playerShader("Resources/Shaders/player.vert", "Resources/Shaders/player.frag"),
      crackShader("Resources/Shaders/crack.vert", "Resources/Shaders/crack.frag") {
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

    // The refraction copy: same formats as the scene so it can be blitted into, with
    // depth as a texture so the water shader can read it.
    glGenFramebuffers(1, &refractFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, refractFBO);
    glGenTextures(1, &refractColor);
    glBindTexture(GL_TEXTURE_2D, refractColor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, refractColor, 0);
    glGenTextures(1, &refractDepth);
    glBindTexture(GL_TEXTURE_2D, refractDepth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, refractDepth, 0);
    complete = complete && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
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
    glDeleteFramebuffers(1, &refractFBO);
    glDeleteTextures(1, &refractColor);
    glDeleteTextures(1, &refractDepth);
    refractFBO = refractColor = refractDepth = 0;
}

void Renderer::ReloadShaders() {
    worldShader.Reload();
    debugShader.Reload();
    skyboxShader.Reload();
    screenShader.Reload();
    holeShader.Reload();
    waterShader.Reload();
    rainShader.Reload();
    playerShader.Reload();
    crackShader.Reload();
}

void Renderer::ReloadTextures() {
    atlas.Build(kBlockTextureDirectory);
}

void Renderer::RenderFrame(ClientWorld& world, const FrameParams& params) {
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
    worldShader.SetUniform("cameraPos", params.cameraPosition);
    SetFogUniforms(worldShader, params, 1);
    world.Render(worldShader, params.view, params.projection, params.time);

    glEnable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    playerShader.Use();
    playerShader.SetUniform("sunDirection", params.sunDirection);
    playerShader.SetUniform("sunBrightness", params.sunBrightness);
    playerShader.SetUniform("blockLightColor", kBlockLightColor);
    playerShader.SetUniform("atlas", 0); // held blocks and items
    atlas.Bind(GL_TEXTURE0);
    playerShader.SetUniform("cameraPos", params.cameraPosition);
    SetFogUniforms(playerShader, params, 1);
    playerModels.Draw(playerShader, params.view, params.projection, params.players);
    playerModels.DrawCreatures(playerShader, params.view, params.projection, params.creatures, params.time);
    playerModels.DrawParticles(playerShader, params.view, params.projection, params.particles);
    playerModels.DrawBolts(playerShader, params.view, params.projection, params.bolts);
    playerModels.DrawArrows(playerShader, params.view, params.projection, params.arrows);

    DrawSkybox(params);

    if (params.breakProgress > 0.0f) DrawCracks(params);

    // The outline of the block in view, before the water: depth tested so blocks in
    // front hide it, and seen through water (which refracts what's behind it).
    if (params.highlightedBlock) {
        debugDraw.DrawOutline(debugShader, params.view, params.projection, glm::vec3(*params.highlightedBlock), params.highlightOutline);
    }

    DrawWater(world, params);

    rain.Draw(rainShader, params.view, params.projection, params.cameraPosition, params.time, params.rain, 0.25f + 0.75f * params.sunBrightness);

    // --- Composite to the default framebuffer ---
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);

    screenShader.Use();
    screenShader.SetUniform("screenTexture", 0);
    screenShader.SetUniform("underwater", params.underwater ? 1.0f : 0.0f);
    glBindVertexArray(screenVAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneColor);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (kDrawBlackHole) DrawBlackHole(params);

    glBindVertexArray(0);
}

void Renderer::DrawWater(ClientWorld& world, const FrameParams& params) {
    // No water in view: skip the full-screen copy as well.
    if (!world.HasVisibleWater()) return;

    // Copy what's behind the water (opaque world and sky) for the shader to sample.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, refractFBO);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);

    // Near/far planes from the projection matrix, to turn depth back into distance.
    const glm::mat4& p = params.projection;
    const float nearPlane = p[3][2] / (p[2][2] - 1.0f);
    const float farPlane = p[3][2] / (p[2][2] + 1.0f);

    waterShader.Use();
    waterShader.SetUniform("atlas", 0);
    atlas.ApplyUniforms(waterShader);
    atlas.Bind(GL_TEXTURE0);
    waterShader.SetUniform("skybox", 1);
    waterShader.SetUniform("sceneColor", 2);
    waterShader.SetUniform("sceneDepth", 3);
    waterShader.SetUniform("screenSize", glm::vec2(width, height));
    waterShader.SetUniform("nearPlane", nearPlane);
    waterShader.SetUniform("farPlane", farPlane);
    waterShader.SetUniform("cameraPos", params.cameraPosition);
    waterShader.SetUniform("sunBrightness", params.sunBrightness);
    waterShader.SetUniform("sunDirection", params.sunDirection);
    waterShader.SetUniform("blockLightColor", kBlockLightColor);
    waterShader.SetUniform("underwater", params.underwater ? 1.0f : 0.0f);
    SetFogUniforms(waterShader, params, 1); // the same cube map as the reflections

    // The surface itself into the scene, opaque: refraction and absorption stand in for
    // blending. Both sides draw so it shows from underwater too.
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, refractColor);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, refractDepth);
    glDisable(GL_CULL_FACE);
    // Water often lies exactly on, or a hair above, the ground it covers: a shore's
    // waterlogged slopes rise to the surface, and thin water sits just over the floor.
    // Depth precision can't separate those (worse at grazing angles), so the ground
    // would poke through in jagged slivers. Pull the water slightly toward the camera
    // so it wins those ties; the offset is within depth precision, so nothing that's
    // really in front of the water is affected. (Open surfaces also stop a little
    // below the top of their cells, so they don't tie with blocks level with them.)
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -2.0f);
    world.RenderWater(waterShader, params.view, params.projection, params.time);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
}

void Renderer::SetFogUniforms(Shader& shader, const FrameParams& params, int skyUnit) {
    // Fog from a little over half the render distance; fully sky by its edge (the
    // nearest the world can end, straight along an axis from the camera's chunk).
    shader.SetUniform("fogEnd", params.fogEnd);
    shader.SetUniform("fogStart", params.fogEnd * 0.55f);
    shader.SetUniform("fogSky", skyUnit);
    shader.SetUniform("skyBrightness", 0.05f + 0.95f * params.sunBrightness);
    shader.SetUniform("skyTint", params.skyTint);
    shader.SetUniform("skyOvercast", params.overcast);
    glActiveTexture(GL_TEXTURE0 + skyUnit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxTexture);
    glActiveTexture(GL_TEXTURE0);
}

void Renderer::DrawCracks(const FrameParams& params) {
    // Over the block, blended, behind anything in front of it; both sides, since the
    // box's winding doesn't matter (the block hides its far faces).
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    crackShader.Use();
    crackShader.SetUniform("view", params.view);
    crackShader.SetUniform("projection", params.projection);
    crackShader.SetUniform("boxMin", params.breakBoxMin);
    crackShader.SetUniform("boxSize", params.breakBoxSize);
    crackShader.SetUniform("progress", params.breakProgress);
    crackShader.SetUniform("brightness", 0.3f + 0.7f * params.sunBrightness);
    glBindVertexArray(skyboxVAO);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
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
    skyboxShader.SetUniform("tint", params.skyTint);
    skyboxShader.SetUniform("overcast", params.overcast);

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
