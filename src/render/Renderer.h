#pragma once

#include <optional>

#include <glad/glad.h>
#include <glm/glm.hpp>

#include "render/DebugDraw.h"
#include "render/Shader.h"
#include "render/TextureAtlas.h"

class World;

struct FrameParams {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    float time = 0.0f;
    bool wireframe = false;
    bool ambientOcclusion = true;
    float sunBrightness = 1.0f; // 0 = night, 1 = day
    glm::vec3 cameraPosition{0.0f};
    bool underwater = false;    // camera is inside water: tint the screen
    std::optional<glm::ivec3> highlightedBlock;
};

// Owns all GPU resources outside the world: shaders, textures, the skybox,
// the offscreen scene framebuffer and the black hole post effect.
// Must be created and destroyed while the GL context is current.
class Renderer
{
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Allocates the offscreen framebuffer at the given size (in pixels).
    bool Init(int framebufferWidth, int framebufferHeight);

    void ReloadShaders();
    // Rebuild the block texture atlas from disk. Tile indices may change, so
    // block textures must be re-resolved and chunk meshes rebuilt afterwards.
    void ReloadTextures();
    const TextureAtlas& GetAtlas() const { return atlas; }
    void RenderFrame(World& world, const FrameParams& params);

    GLuint GetSceneTexture() const { return sceneColor; }
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }

private:
    void CreateGeometry();
    bool CreateSceneFramebuffer();
    void DestroySceneFramebuffer();

    void DrawSkybox(const FrameParams& params);
    void DrawBlackHole(const FrameParams& params);
    void DrawWater(World& world, const FrameParams& params);

    Shader worldShader;
    Shader debugShader;
    Shader skyboxShader;
    Shader screenShader;
    Shader holeShader;
    Shader waterShader;

    DebugDraw debugDraw;
    TextureAtlas atlas;

    int width = 0, height = 0;

    GLuint skyboxTexture = 0;

    GLuint skyboxVAO = 0, skyboxVBO = 0;
    GLuint screenVAO = 0, screenVBO = 0;
    GLuint holeVAO = 0, holeVBO = 0;

    GLuint sceneFBO = 0, sceneColor = 0, sceneDepth = 0;
    // Copy of the scene taken before water is drawn, which the water shader samples for
    // refraction and to measure how much water lies in front of what's behind it.
    GLuint refractFBO = 0, refractColor = 0, refractDepth = 0;
};
