#include "app/Application.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <string>

#include <glad/glad.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "render/Renderer.h"
#include "world/Chunk.h"
#include "world/Raycast.h"
#include "world/World.h"

namespace {

constexpr int kWindowWidth = 800;
constexpr int kWindowHeight = 600;
constexpr const char* kGlslVersion = "#version 330 core";

constexpr float kReach = 50.0f;
constexpr float kLaserRange = 100.0f;
constexpr int kLaserRadius = 4;
// Blocks selectable with the number keys, in order.
constexpr BlockType kPlaceableBlocks[] = { BlockType::DIRT, BlockType::STONE, BlockType::GRASS, BlockType::LAMP, BlockType::LOG, BlockType::LEAVES, BlockType::SAND, BlockType::WATER };

const char* BlockName(BlockType type) {
    switch (type) {
    case BlockType::GRASS: return "Grass";
    case BlockType::DIRT:  return "Dirt";
    case BlockType::STONE: return "Stone";
    case BlockType::LAMP:  return "Lamp";
    case BlockType::LOG:   return "Log";
    case BlockType::LEAVES: return "Leaves";
    case BlockType::TALL_GRASS: return "Tall grass";
    case BlockType::FLOWER_RED: return "Red flower";
    case BlockType::FLOWER_YELLOW: return "Yellow flower";
    case BlockType::SAND:  return "Sand";
    case BlockType::WATER: return "Water";
    default:               return "Air";
    }
}

void GlfwErrorCallback(int error, const char* description) {
    std::cerr << "GLFW error " << error << ": " << description << std::endl;
}

} // namespace

Application::Application() = default;

Application::~Application() {
    Shutdown();
}

bool Application::Init() {
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit()) {
        std::cerr << "ERROR: Failed to initialize GLFW" << std::endl;
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    window = glfwCreateWindow(kWindowWidth, kWindowHeight, "VoxelEngine", nullptr, nullptr);
    if (!window) {
        std::cerr << "ERROR: Failed to create window" << std::endl;
        return false;
    }

    glfwMakeContextCurrent(window);
    if (!gladLoadGL()) {
        std::cerr << "ERROR: Failed to initialize GLAD" << std::endl;
        return false;
    }

    glfwSwapInterval(vsync ? 1 : 0);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    // Installed before ImGui so its backend chains to ours.
    glfwSetWindowUserPointer(window, this);
    glfwSetKeyCallback(window, KeyCallback);
    glfwSetMouseButtonCallback(window, MouseButtonCallback);
    glfwSetCursorPosCallback(window, CursorPosCallback);
    glfwSetScrollCallback(window, ScrollCallback);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(kGlslVersion);
    imguiInitialized = true;

    // On high-DPI displays the framebuffer is larger than the window.
    int framebufferWidth, framebufferHeight;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    renderer = std::make_unique<Renderer>();
    if (!renderer->Init(framebufferWidth, framebufferHeight)) return false;

    // Block textures must be resolved before the world starts meshing.
    LoadBlockTextures();
    world = std::make_unique<World>(terrain);
    world->StartWaterThread();
    return true;
}

void Application::LoadBlockTextures() {
    const TextureAtlas& atlas = renderer->GetAtlas();
    ResolveBlockTextures([&](const std::string& name) {
        int index = atlas.GetIndex(name);
        if (index == TextureAtlas::kMissingIndex)
            std::cerr << "Missing block texture '" << name << "'" << std::endl;
        return index;
    });
}

void Application::Shutdown() {
    // Order matters: everything holding GL objects goes before the context.
    world.reset();
    renderer.reset();

    if (imguiInitialized) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized = false;
    }

    if (window) {
        glfwDestroyWindow(window);
        window = nullptr;
    }
    glfwTerminate();
}

void Application::Run() {
    double lastFrame = glfwGetTime();
    bool appliedVsync = vsync;

    while (!glfwWindowShouldClose(window)) {
        const double now = glfwGetTime();
        const float deltaTime = static_cast<float>(now - lastFrame);
        lastFrame = now;

        glfwPollEvents();

        HandleContinuousInput(deltaTime);
        camera.SetPosition(player.GetEyePosition());
        world->Update(camera.GetPosition());

        FrameParams params;
        params.view = camera.GetViewMatrix();
        params.projection = camera.GetProjectionMatrix(static_cast<float>(renderer->GetWidth()) / renderer->GetHeight());
        params.time = static_cast<float>(now);
        params.wireframe = wireframe;
        params.ambientOcclusion = ambientOcclusion;
        params.sunBrightness = sunBrightness;
        params.cameraPosition = camera.GetPosition();
        params.underwater = world->IsUnderwater(camera.GetPosition());
        if (auto hit = Raycast(*world, camera.GetPosition(), camera.GetDirection(), kReach)) {
            params.highlightedBlock = hit->block;
        }

        renderer->RenderFrame(*world, params);

        // With the cursor captured it's hidden but still moves; keep it from clicking
        // on the UI by accident.
        ImGuiIO& io = ImGui::GetIO();
        if (captureCursor) io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        DrawHotbar();
        if (showDebug) DrawDebugWindow(deltaTime);
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (vsync != appliedVsync) {
            glfwSwapInterval(vsync ? 1 : 0);
            appliedVsync = vsync;
        }
        glfwSwapBuffers(window);
    }
}

void Application::HandleContinuousInput(float deltaTime) {
    auto down = [&](int key) { return glfwGetKey(window, key) == GLFW_PRESS; };

    // The player drops onto the terrain once the chunks under it have loaded.
    if (!player.IsSpawned() && !player.IsFlying()) player.Spawn(*world);

    PlayerInput input;
    input.move = glm::vec2((down(GLFW_KEY_D) ? 1.0f : 0.0f) - (down(GLFW_KEY_A) ? 1.0f : 0.0f),
                           (down(GLFW_KEY_W) ? 1.0f : 0.0f) - (down(GLFW_KEY_S) ? 1.0f : 0.0f));
    input.jump = down(GLFW_KEY_SPACE) || (player.IsFlying() && down(GLFW_KEY_E));
    input.descend = down(GLFW_KEY_Q) || down(GLFW_KEY_LEFT_CONTROL);
    input.sprint = down(GLFW_KEY_LEFT_SHIFT);
    input.yaw = camera.GetYaw();
    input.lookDirection = camera.GetDirection();
    player.Update(*world, input, deltaTime);

    // Laser: carve a tunnel along the view ray while L is held.
    if (captureCursor && glfwGetKey(window, GLFW_KEY_L) == GLFW_PRESS) {
        const int r = kLaserRadius;
        for (const auto& center : TraceBlocks(*world, camera.GetPosition(), camera.GetDirection(), kLaserRange)) {
            for (int x = -r; x <= r; x++)
                for (int y = -r; y <= r; y++)
                    for (int z = -r; z <= r; z++)
                        if (x * x + y * y + z * z <= r * r)
                            world->SetBlock(center.x + x, center.y + y, center.z + z, BlockType::AIR);
        }
    }
}

void Application::DrawHotbar() {
    constexpr int kSlots = static_cast<int>(std::size(kPlaceableBlocks));
    constexpr float kIcon = 40.0f, kPad = 6.0f, kGap = 4.0f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float barWidth = kSlots * (kIcon + 2 * kPad) + (kSlots - 1) * kGap;

    // A borderless strip along the bottom centre of the screen.
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y - 12.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kGap, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("Hotbar", nullptr, flags)) {
        // Name of the selected block, centred above the slots.
        const char* name = BlockName(SelectedBlock());
        const float nameWidth = ImGui::CalcTextSize(name).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (barWidth - nameWidth) * 0.5f);
        ImGui::TextUnformatted(name);

        const TextureAtlas& atlas = renderer->GetAtlas();
        const ImTextureID atlasTexture = static_cast<ImTextureID>(static_cast<uintptr_t>(atlas.GetTexture()));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (int i = 0; i < kSlots; i++) {
            if (i > 0) ImGui::SameLine();
            const bool selected = i == selectedSlot;
            const ImVec2 slotMin = ImGui::GetCursorScreenPos();
            const ImVec2 slotMax(slotMin.x + kIcon + 2 * kPad, slotMin.y + kIcon + 2 * kPad);

            // Clicking a slot selects it (when the cursor is released with Tab).
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("slot", ImVec2(slotMax.x - slotMin.x, slotMax.y - slotMin.y))) SelectSlot(i);
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            draw->AddRectFilled(slotMin, slotMax, selected ? IM_COL32(255, 255, 255, 60) : hovered ? IM_COL32(255, 255, 255, 30) : IM_COL32(0, 0, 0, 70), 4.0f);
            // The block's side texture, straight from the world's atlas.
            glm::vec2 uv0, uv1;
            atlas.GetTileUV(GetTextureData(kPlaceableBlocks[i]).front, uv0, uv1);
            draw->AddImage(atlasTexture, ImVec2(slotMin.x + kPad, slotMin.y + kPad), ImVec2(slotMax.x - kPad, slotMax.y - kPad),
                           ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv1.y));
            draw->AddRect(slotMin, slotMax, selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 60), 4.0f, 0, selected ? 3.0f : 1.0f);
            // Number key in the corner.
            char key[2] = { static_cast<char>('1' + i), 0 };
            draw->AddText(ImVec2(slotMin.x + 5.0f, slotMin.y + 3.0f), IM_COL32(0, 0, 0, 200), key); // shadow, readable on light textures
            draw->AddText(ImVec2(slotMin.x + 4.0f, slotMin.y + 2.0f), IM_COL32(255, 255, 255, selected ? 255 : 190), key);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void Application::DrawDebugWindow(float deltaTime) {
    frameSampleTimer += deltaTime;
    if (frameSampleTimer > 0.01f) {
        frameTimes[frameTimeIndex] = deltaTime * 1000.0f;
        frameTimeIndex = (frameTimeIndex + 1) % kFrameHistory;
        frameSampleTimer = 0.0f;
    }

    if (ImGui::Begin("Debug", &showDebug)) {
        const glm::vec3 pos = camera.GetPosition();
        const glm::vec2 angles = camera.GetDirectionAngles();

        ImGui::Checkbox("VSync", &vsync);
        ImGui::Checkbox("Wireframe", &wireframe);
        ImGui::Checkbox("Ambient occlusion", &ambientOcclusion);
        ImGui::SliderFloat("Sun brightness", &sunBrightness, 0.0f, 1.0f);
        ImGui::Text("Placing: %s", BlockName(SelectedBlock()));
        ImGui::Text("Position %.3f, %.3f, %.3f", pos.x, pos.y, pos.z);
        const glm::vec3 velocity = player.GetVelocity();
        ImGui::Text("%s%s, velocity %.2f, %.2f, %.2f", player.IsFlying() ? "Flying" : player.IsSwimming() ? "Swimming" : "Walking",
                    player.IsFlying() || player.IsSwimming() ? "" : player.IsOnGround() ? " (on ground)" : " (airborne)",
                    velocity.x, velocity.y, velocity.z);
        ImGui::Text("Angles %.3f, %.3f", angles.x, angles.y);
        ImGui::Text("Frame %.3f ms (%.1f FPS)", deltaTime * 1000.0f, deltaTime > 0.0f ? 1.0f / deltaTime : 0.0f);
        ImGui::PlotLines("Frame Time (ms)", frameTimes.data(), kFrameHistory, frameTimeIndex);
        ImGui::Text("Chunks: %zu in world, %d alive", world->GetLoadedChunkCount(), Chunk::GetLiveCount());
        ImGui::Text("Active water cells: %zu", world->GetWater().GetActiveCount());

        const TextureAtlas& atlas = renderer->GetAtlas();
        if (ImGui::CollapsingHeader("Texture atlas")) {
            glm::ivec2 size = atlas.GetSize();
            ImGui::Text("%zu textures, %dx%d, %dpx tiles (R to reload)", atlas.GetNames().size() - 1, size.x, size.y, atlas.GetTileSize());
            const float scale = 128.0f / std::max(size.x, size.y);
            ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(atlas.GetTexture())), ImVec2(size.x * scale, size.y * scale));
        }

        const float previewScale = 0.25f;
        ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(renderer->GetSceneTexture())),
                     ImVec2(renderer->GetWidth() * previewScale, renderer->GetHeight() * previewScale),
                     ImVec2(0, 1), ImVec2(1, 0));
    }
    ImGui::End();
}

void Application::OnKey(int key, int action) {
    if (action != GLFW_PRESS) return;

    switch (key) {
    case GLFW_KEY_ESCAPE:
        glfwSetWindowShouldClose(window, GLFW_TRUE);
        break;
    case GLFW_KEY_R:
        renderer->ReloadShaders();
        renderer->ReloadTextures();
        LoadBlockTextures();
        world->RebuildAllChunks();
        break;
    case GLFW_KEY_TAB:
        captureCursor = !captureCursor;
        glfwSetInputMode(window, GLFW_CURSOR, captureCursor ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        camera.ResetLook();
        break;
    case GLFW_KEY_F1:
        wireframe = !wireframe;
        break;
    case GLFW_KEY_F2:
        showDebug = !showDebug;
        break;
    case GLFW_KEY_F3:
        ambientOcclusion = !ambientOcclusion;
        break;
    case GLFW_KEY_F:
        player.SetFlying(*world, !player.IsFlying());
        break;
    case GLFW_KEY_N:
        sunBrightness = sunBrightness > 0.5f ? 0.0f : 1.0f;
        break;
    default:
        if (key >= GLFW_KEY_1 && key < GLFW_KEY_1 + static_cast<int>(std::size(kPlaceableBlocks))) {
            SelectSlot(key - GLFW_KEY_1);
        }
        break;
    }
}

BlockType Application::SelectedBlock() const {
    return kPlaceableBlocks[selectedSlot];
}

void Application::SelectSlot(int slot) {
    const int count = static_cast<int>(std::size(kPlaceableBlocks));
    selectedSlot = ((slot % count) + count) % count;
}

void Application::OnMouseButton(int button, int action) {
    // With the cursor released, clicks belong to the UI.
    if (action != GLFW_PRESS || !captureCursor) return;

    auto hit = Raycast(*world, camera.GetPosition(), camera.GetDirection(), kReach);
    if (!hit) return;

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        // Breaking a waterlogged block leaves its water behind.
        Block broken;
        world->GetBlock(hit->block.x, hit->block.y, hit->block.z, broken);
        world->SetBlock(hit->block.x, hit->block.y, hit->block.z, broken.HasWater() ? Block::Water(broken.WaterVolume()) : Block());
    }
    else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        const glm::ivec3 place = hit->block + hit->normal;
        // Don't build a block inside yourself when walking (water is fine).
        const BlockType placeBlock = SelectedBlock();
        if (!player.IsFlying() && placeBlock != BlockType::WATER) {
            const Collision::AABB body = player.GetBounds();
            const glm::vec3 lo(place), hi = lo + glm::vec3(1.0f);
            if (glm::all(glm::lessThan(lo, body.max)) && glm::all(glm::greaterThan(hi, body.min))) return;
        }
        world->SetBlock(place.x, place.y, place.z, placeBlock == BlockType::WATER ? Block::Water(Block::kMaxWaterVolume) : Block(placeBlock));
    }
}

void Application::OnCursorMove(double x, double y) {
    if (captureCursor) camera.UpdateLook(x, y);
}

void Application::OnScroll(double yOffset) {
    // With the cursor released the wheel belongs to the UI.
    if (!captureCursor) return;
    // Trackpads send many small steps; change slot once per whole notch.
    scrollAccumulator += yOffset;
    while (scrollAccumulator >= 1.0) { SelectSlot(selectedSlot - 1); scrollAccumulator -= 1.0; }
    while (scrollAccumulator <= -1.0) { SelectSlot(selectedSlot + 1); scrollAccumulator += 1.0; }
}

void Application::ScrollCallback(GLFWwindow* window, double, double yOffset) {
    static_cast<Application*>(glfwGetWindowUserPointer(window))->OnScroll(yOffset);
}

void Application::KeyCallback(GLFWwindow* window, int key, int, int action, int) {
    static_cast<Application*>(glfwGetWindowUserPointer(window))->OnKey(key, action);
}

void Application::MouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    static_cast<Application*>(glfwGetWindowUserPointer(window))->OnMouseButton(button, action);
}

void Application::CursorPosCallback(GLFWwindow* window, double x, double y) {
    static_cast<Application*>(glfwGetWindowUserPointer(window))->OnCursorMove(x, y);
}
