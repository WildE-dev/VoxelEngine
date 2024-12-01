#include "Renderer.h"
#include "AssetLoader.h"
#include "ShaderResources.h"
#include "TextureResources.h"

Renderer::Renderer() : framebuffer(0), textureColorbuffer(0), rbo(0) {
    // Initialize pointers to nullptr
    for (int i = 0; i < 5; ++i) {
        shaders[i] = nullptr;
    }
}

Renderer::~Renderer() {
    Cleanup();
}

void Renderer::Initialize(int frameWidth, int frameHeight) {
    resolution = glm::vec2(frameWidth, frameHeight);

    // Load shaders
    shaders[0] = new Shader(main_vert, main_vert_size, main_frag, main_frag_size);
    shaders[1] = new Shader(debug_vert, debug_vert_size, debug_frag, debug_frag_size);
    shaders[2] = new Shader(skybox_vert, skybox_vert_size, skybox_frag, skybox_frag_size);
    shaders[3] = new Shader(screen_vert, screen_vert_size, screen_frag, screen_frag_size);
    shaders[4] = new Shader(hole_vert, hole_vert_size, hole_frag, hole_frag_size);

    // Load cubemap texture
    std::array<const unsigned char*, 6> faces{
        Daylight_Box_Right_bmp, Daylight_Box_Left_bmp, Daylight_Box_Top_bmp,
        Daylight_Box_Bottom_bmp, Daylight_Box_Front_bmp, Daylight_Box_Back_bmp,
    };
    std::array<const unsigned int, 6> sizes{
        Daylight_Box_Right_bmp_size, Daylight_Box_Left_bmp_size, Daylight_Box_Top_bmp_size,
        Daylight_Box_Bottom_bmp_size, Daylight_Box_Front_bmp_size, Daylight_Box_Back_bmp_size,
    };
    cubemapTexture = AssetLoader::loadCubemap(faces, sizes);

    // Load atlas
    atlasTexture = AssetLoader::loadTexture(atlas_png, atlas_png_size);

    SetupCube();
    SetupSkybox();
    SetupFramebuffer(frameWidth, frameHeight);
    SetupScreenQuad();
    SetupHole();

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW);
}

void Renderer::SetupCube() {
    glGenVertexArrays(1, &cubeVAO);
    glGenBuffers(1, &cubeVBO);

    glBindVertexArray(cubeVAO);

    float triVerts[] = {
        0,0,0,
        1,0,0,
        1,1,0,

        0,0,0,
        1,1,0,
        0,1,0,

        0,1,0,
        1,1,0,
        1,1,1,

        0,1,0,
        1,1,1,
        0,1,1,

        0,0,0,
        0,1,1,
        0,0,1,

        0,0,0,
        0,1,0,
        0,1,1,

        0,0,0,
        1,0,1,
        1,0,0,

        0,0,0,
        0,0,1,
        1,0,1,

        0,0,1,
        1,1,1,
        1,0,1,

        0,0,1,
        0,1,1,
        1,1,1,

        1,0,0,
        1,0,1,
        1,1,1,

        1,0,0,
        1,1,1,
        1,1,0,
    };

    float lineVerts[] = {
        0,0,0,
        1,0,0,

        1,0,0,
        1,1,0,

        1,1,0,
        0,1,0,

        0,1,0,
        0,0,0,

        0,0,1,
        1,0,1,

        1,0,1,
        1,1,1,

        1,1,1,
        0,1,1,

        0,1,1,
        0,0,1,

        0,0,0,
        0,0,1,

        0,1,0,
        0,1,1,

        1,0,0,
        1,0,1,

        1,1,0,
        1,1,1
    };

    glBindBuffer(GL_ARRAY_BUFFER, cubeVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(lineVerts), lineVerts, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}

void Renderer::SetupSkybox() {
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
}

void Renderer::SetupFramebuffer(int frameWidth, int frameHeight) {
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    // create a color attachment texture
    glGenTextures(1, &textureColorbuffer);
    glBindTexture(GL_TEXTURE_2D, textureColorbuffer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frameWidth, frameHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureColorbuffer, 0);

    // create a renderbuffer object for depth and stencil attachment (we won't be sampling these)
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, frameWidth, frameHeight); // use a single renderbuffer object for both a depth AND stencil buffer.
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo); // now actually attach it
    // now that we actually created the framebuffer and added all attachments we want to check if it is actually complete now
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cout << "ERROR::FRAMEBUFFER:: Framebuffer is not complete!" << std::endl;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::SetupScreenQuad() {
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
}

void Renderer::SetupHole() {
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
}

void Renderer::RenderWorld(World& world, Camera& camera, glm::mat4 view, glm::mat4 projection) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlasTexture);
    world.Render(*shaders[0], view, projection, resolution.x, resolution.y);
}

void Renderer::RenderSkybox(glm::mat4 view, glm::mat4 projection) {
    shaders[2]->Use();
    shaders[2]->SetUniform("view", glm::mat4(glm::mat3(view)));
    shaders[2]->SetUniform("projection", projection);

    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);

    glBindVertexArray(skyboxVAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cubemapTexture);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
}

void Renderer::DrawCube(glm::mat4 view, glm::mat4 projection, glm::vec3 pos)
{
    shaders[1]->Use();

    glm::mat4 model = glm::mat4(1.0f);
    shaders[1]->SetUniform("model", model);
    shaders[1]->SetUniform("projection", projection);

    glm::mat4 newViewMatrix = glm::translate(view, pos);
    shaders[1]->SetUniform("view", newViewMatrix);

    glDisable(GL_DEPTH_TEST);

    glBindVertexArray(cubeVAO);
    glDrawArrays(GL_LINES, 0, 24);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
}

void Renderer::RenderScreen() {
    shaders[3]->Use();
    glBindVertexArray(screenVAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureColorbuffer);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void Renderer::RenderHole(glm::mat4 view, glm::mat4 projection) {
    shaders[4]->Use();
    glm::mat4 model = glm::mat4(1);
    model = glm::translate(model, glm::vec3(0, 2, 0));
    shaders[4]->SetUniform("model", model);
    shaders[4]->SetUniform("view", view);
    shaders[4]->SetUniform("projection", projection);
    shaders[4]->SetUniform("uResolution", glm::vec2(resolution.x, resolution.y));
    
    const float eventHorizon = 0.25f;
    const float lensingRadius = 0.4f;
    const float maxDistortion = 0.3f;

    shaders[4]->SetUniform("eventHorizon", eventHorizon);
    shaders[4]->SetUniform("lensingRadius", lensingRadius);
    shaders[4]->SetUniform("maxDistortion", maxDistortion);

    glBindVertexArray(holeVAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureColorbuffer);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);
}

void Renderer::Render(World& world, Camera& camera, DebugMenu& debugMenu, double deltaTime) {
    // Start rendering to the framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    glViewport(0, 0, resolution.x, resolution.y);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);

    // Wireframe mode
    glPolygonMode(GL_FRONT_AND_BACK, debugMenu.GetWireframe() ? GL_LINE : GL_FILL);
    if (debugMenu.GetWireframe()) glDisable(GL_CULL_FACE); else glEnable(GL_CULL_FACE);

    glm::mat4 view = camera.GetViewMatrix();
    glm::mat4 projection = camera.GetProjectionMatrix(resolution.x, resolution.y);

    RenderWorld(world, camera, view, projection);

    // Only the world should be wireframe
    glEnable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Render skybox
    RenderSkybox(view, projection);

    glm::ivec3 pos;
    glm::vec3 norm;
    if (world.TraceRay(camera.GetPosition(), camera.GetDirection(), 50, pos, norm)) {
        DrawCube(view, projection, pos);
    }

    // Start rendering to the screen
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);

    RenderScreen();

    RenderHole(view, projection);

    debugMenu.Render(camera, deltaTime);
}

void Renderer::Cleanup() {
    glDeleteVertexArrays(1, &skyboxVAO);
    glDeleteBuffers(1, &skyboxVBO);
    glDeleteVertexArrays(1, &cubeVAO);
    glDeleteBuffers(1, &cubeVBO);
    glDeleteVertexArrays(1, &screenVAO);
    glDeleteBuffers(1, &screenVBO);
    glDeleteVertexArrays(1, &holeVAO);
    glDeleteBuffers(1, &holeVBO);
    glDeleteFramebuffers(1, &rbo);
}

void Renderer::ReloadShaders() {
    for (size_t i = 0; i < sizeof(shaders) / sizeof(Shader*); i++)
    {
        shaders[i]->ReloadShader();
    }
}