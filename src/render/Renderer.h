#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <glad/glad.h>
#include <glm/glm.hpp>

#include "render/DebugDraw.h"
#include "render/Rain.h"
#include "render/PlayerModels.h"
#include "render/Shader.h"
#include "render/TextureAtlas.h"

class ClientWorld;

struct FrameParams {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    float time = 0.0f;
    bool wireframe = false;
    bool ambientOcclusion = true;
    float sunBrightness = 1.0f; // 0 = night, 1 = day (already dimmed by rain)
    glm::vec3 sunDirection{0.35f, 0.75f, 0.55f}; // towards the sun, normalised
    glm::vec3 skyTint{1.0f};    // colour of the sky (warm at dawn and dusk)
    float rain = 0.0f;          // 0..1: how hard it rains here (0 when sheltered)
    bool snow = false;          // it falls as snow here (cold ground around)
    float rainbow = 0.0f;       // 0..1: a rainbow opposite the sun (a shower clearing by day)
    float overcast = 0.0f;      // 0..1: how grey the sky is
    glm::vec3 cameraPosition{0.0f};
    bool underwater = false;    // camera is inside water: tint the screen
    float fogEnd = 0.0f;        // horizontal distance where the world fades fully into the sky (0: no fog)
    std::optional<glm::ivec3> highlightedBlock;
    // Mining (survival): cracks over the box around the block being broken, 0..1 of the way.
    float breakProgress = 0.0f;
    glm::vec3 breakBoxMin{0.0f}, breakBoxSize{1.0f};
    std::vector<glm::vec3> highlightOutline; // the block's shape (Block::OutlineSegments)
    std::vector<PlayerModel> players; // other players
    std::vector<CreatureModel> creatures;
    std::vector<ParticleModel> particles; // bits of broken blocks
    std::vector<BoltModel> bolts;         // lightning
    std::vector<ArrowModel> arrows;
    std::vector<WheelModel> wheels;
    std::vector<GaugeModel> gauges;
    std::vector<GlowModel> glows; // fireflies
    std::vector<glm::vec4> handLights; // lights carried in hand: position, and strength (a light level)
};

// Owns all GPU resources outside the world: shaders, textures, the skybox,
// the offscreen scene framebuffer and the black hole post effect (off by default,
// see kDrawBlackHole).
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
    void RenderFrame(ClientWorld& world, const FrameParams& params);

    GLuint GetSceneTexture() const { return sceneColor; }
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }

private:
    void CreateGeometry();
    bool CreateSceneFramebuffer();
    void DestroySceneFramebuffer();

    void DrawSkybox(const FrameParams& params);
    // Distance fog for a shader that includes ApplyFog; the skybox goes to texture
    // unit `skyUnit` (left bound there).
    void SetFogUniforms(Shader& shader, const FrameParams& params, int skyUnit);
    void DrawBlackHole(const FrameParams& params);
    void DrawWater(ClientWorld& world, const FrameParams& params);
    void DrawCracks(const FrameParams& params);

    Shader worldShader;
    Shader debugShader;
    Shader skyboxShader;
    Shader screenShader;
    Shader holeShader;
    Shader waterShader;
    Shader rainShader;
    Shader playerShader;
    Shader crackShader;

    DebugDraw debugDraw;
    Rain rain;
    PlayerModels playerModels;
public:
    bool LoadCreatureModels(std::string& error) { return playerModels.LoadCreatureModels(PlayerModels::kCreatureModelsPath, error); }
private:
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
