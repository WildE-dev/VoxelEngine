#include "app/Application.h"

#include <algorithm>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <glad/glad.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "client/Client.h"
#include "client/ClientWorld.h"
#include "net/Protocol.h"
#include "render/Renderer.h"
#include "server/Server.h"
#include "world/Breathing.h"
#include "world/Chunk.h"
#include "world/Raycast.h"
#include "world/TerrainGenerator.h"

namespace {

constexpr int kWindowWidth = 800;
constexpr int kWindowHeight = 600;
constexpr const char* kGlslVersion = "#version 330 core";

constexpr float kReach = 50.0f;
constexpr float kLaserRange = 100.0f;
constexpr int kLaserRadius = 4;
// Everything the inventory (I) offers, in the order it shows them.
constexpr BlockType kInventoryBlocks[] = {
    BlockType::DIRT, BlockType::GRASS, BlockType::DRY_GRASS, BlockType::LUSH_GRASS, BlockType::SNOWY_GRASS, BlockType::STONE, BlockType::SAND,
    BlockType::SNOW, BlockType::ICE, BlockType::LOG, BlockType::LEAVES, BlockType::PINE_LEAVES, BlockType::CACTUS, BlockType::LAMP,
    BlockType::STONE_BRICK, BlockType::MOSSY_BRICK, BlockType::CRUMBLING_BRICK, BlockType::WATER, BlockType::PUMP, BlockType::PIPE,
    BlockType::SLUICE, BlockType::PRESSURE_PLATE, BlockType::SPIKES, BlockType::TALL_GRASS, BlockType::FLOWER_RED, BlockType::FLOWER_YELLOW,
    BlockType::DEAD_BUSH,
};
// Where the chosen hotbar is kept between games (working directory).
constexpr const char* kHotbarFile = "hotbar.txt";

// Atlas tile shown for a block in the hotbar and inventory: a sprite's own picture, a
// block's side.
// Resource units as blocks: "12", "3.5", "0.25".
std::string FormatBlocks(uint32_t units) {
    char text[32];
    std::snprintf(text, sizeof text, "%.2f", units / static_cast<double>(Block::kUnitsPerBlock));
    std::string s = text;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s;
}

const char* HeightName(int eighths) {
    switch (eighths) {
    case 2: return "1/4";
    case 4: return "1/2";
    case 6: return "3/4";
    default: return "full";
    }
}

int IconTile(BlockType type) {
    const TextureData t = GetTextureData(type);
    return IsPlantType(type) ? t.top : t.front;
}

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
    case BlockType::PUMP:  return "Pump";
    case BlockType::PIPE:  return "Pipe";
    case BlockType::STONE_BRICK: return "Stone bricks";
    case BlockType::MOSSY_BRICK: return "Mossy bricks";
    case BlockType::SPIKES: return "Spikes";
    case BlockType::CRUMBLING_BRICK: return "Crumbling bricks";
    case BlockType::PRESSURE_PLATE: return "Pressure plate";
    case BlockType::SLUICE: return "Sluice gate";
    case BlockType::SNOW: return "Snow";
    case BlockType::DRY_GRASS: return "Dry grass";
    case BlockType::LUSH_GRASS: return "Lush grass";
    case BlockType::SNOWY_GRASS: return "Snowy grass";
    case BlockType::PINE_LEAVES: return "Pine needles";
    case BlockType::CACTUS: return "Cactus";
    case BlockType::DEAD_BUSH: return "Dead bush";
    case BlockType::ICE: return "Ice";
    default:               return "Air";
    }
}

// Saves the default framebuffer's back buffer (the frame about to be shown) as a PNG.
void SaveScreenshot(const std::string& path, int width, int height) {
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    stbi_flip_vertically_on_write(1);
    if (stbi_write_png(path.c_str(), width, height, 3, pixels.data(), width * 3)) std::cout << "Saved " << path << std::endl;
    else std::cerr << "Could not save " << path << std::endl;
}

void GlfwErrorCallback(int error, const char* description) {
    std::cerr << "GLFW error " << error << ": " << description << std::endl;
}

} // namespace

Application::Application() = default;

Application::~Application() {
    Shutdown();
}

bool Application::Init(const ClientOptions& options) {
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

    // For unattended checks: VOXEL_AUTO_SCREENSHOT="seconds:path.png" saves one
    // screenshot that long after start, and VOXEL_START_YAW sets the initial heading.
    if (const char* shot = std::getenv("VOXEL_AUTO_SCREENSHOT")) {
        const std::string spec = shot;
        const size_t colon = spec.find(':');
        if (colon != std::string::npos) {
            autoScreenshotAt = std::atof(spec.substr(0, colon).c_str());
            autoScreenshotPath = spec.substr(colon + 1);
        }
    }
    // VOXEL_NO_MOUSE=1 ignores mouse look, so automated screenshots from a saved
    // position always face the same way.
    ignoreMouse = std::getenv("VOXEL_NO_MOUSE") != nullptr;
    // VOXEL_BENCH=1 turns vsync off and prints where frame time goes, once a second.
    if (std::getenv("VOXEL_BENCH")) {
        bench = true;
        vsync = false;
        glfwSwapInterval(0);
    }
    if (const char* yaw = std::getenv("VOXEL_START_YAW")) camera.SetYaw(static_cast<float>(std::atof(yaw)));

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
    LoadHotbar();
    // VOXEL_OPEN_INVENTORY=1 starts with the inventory open (for screenshots).
    if (std::getenv("VOXEL_OPEN_INVENTORY")) SetInventoryOpen(true);
    world = std::make_unique<ClientWorld>();
    client = std::make_unique<Client>(*world);

    // Singleplayer runs a server in this process and connects to it like any other
    // client, over the loopback interface on a free port.
    std::string host = options.host;
    uint16_t port = options.port ? options.port : net::kDefaultPort;
    if (host.empty()) {
        localServer = std::make_unique<Server>();
        Server::Options serverOptions;
        serverOptions.bindHost = "127.0.0.1";
        serverOptions.port = options.port; // 0: any free port
        serverOptions.worldDirectory = options.worldDirectory;
        serverOptions.waterSimulationDistance = options.waterSimulationDistance;
        if (options.emptyWorld) serverOptions.worldType = TerrainGenerator::Type::Empty;
        serverOptions.dayLengthSeconds = options.dayLengthSeconds;
        serverOptions.ownerName = options.name; // it's your world: you're its operator
        serverOptions.weather = static_cast<Server::Options::Weather>(options.weather);
        serverOptions.defaultMode = options.mode >= 0 ? static_cast<net::GameMode>(options.mode)
                                    : options.emptyWorld ? net::GameMode::Creative : net::GameMode::Survival;
        if (!localServer->Start(serverOptions)) return false;
        localServer->RunInBackground();
        host = "127.0.0.1";
        port = localServer->GetPort();
        serverLabel = "singleplayer";
    }
    else {
        serverLabel = host + ":" + std::to_string(port);
    }

    std::string error;
    if (!client->Connect(host, port, options.name, error)) {
        std::cerr << "ERROR: Could not connect to " << host << ":" << port << ": " << error << std::endl;
        return false;
    }
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
    if (client) client->Disconnect();
    client.reset();
    world.reset();
    renderer.reset();
    if (localServer) localServer->Stop();
    localServer.reset();

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

        double mark = glfwGetTime();
        auto lap = [&](int section) {
            if (!bench) return;
            const double t = glfwGetTime();
            benchSections[section] += t - mark;
            mark = t;
        };
        UpdateNetwork();
        lap(0);
        HandleContinuousInput(deltaTime);
        camera.SetPosition(player.GetEyePosition());
        world->Update(camera.GetPosition());
        lap(1);

        FrameParams params;
        params.view = camera.GetViewMatrix();
        params.projection = camera.GetProjectionMatrix(static_cast<float>(renderer->GetWidth()) / renderer->GetHeight());
        params.time = static_cast<float>(now);
        params.wireframe = wireframe;
        params.ambientOcclusion = ambientOcclusion;
        params.cameraPosition = camera.GetPosition();
        params.underwater = world->IsUnderwater(camera.GetPosition());
        params.fogEnd = static_cast<float>(world->GetViewDistance() * Chunk::CHUNK_SIZE);
        UpdateSky(deltaTime, params);
        if (auto hit = Raycast(*world, camera.GetPosition(), camera.GetDirection(), kReach)) {
            params.highlightedBlock = hit->block;
            Block target;
            if (world->GetBlock(hit->block.x, hit->block.y, hit->block.z, target)) params.highlightOutline = target.OutlineSegments();
        }
        for (const auto& other : client->GetRemotePlayers()) {
            params.players.push_back({ other.feet, other.yaw, other.pitch, PlayerModels::ColorFor(other.id) });
        }

        lap(2);
        renderer->RenderFrame(*world, params);
        lap(3);
        if (bench) glFinish(); // wait for the GPU, so its time shows up as its own section
        lap(6);

        // With the cursor captured it's hidden but still moves; keep it from clicking
        // on the UI by accident.
        ImGuiIO& io = ImGui::GetIO();
        if (captureCursor) io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        DrawHotbar();
        DrawStatus();
        DrawChat();
        DrawCrosshair();
        DrawHealth();
        DrawInventory();
        if (showDebug) DrawDebugWindow(deltaTime);
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        lap(4);

        if (screenshotRequested || (!autoScreenshotPath.empty() && now >= autoScreenshotAt)) {
            std::string path = autoScreenshotPath;
            if (screenshotRequested) {
                char name[64];
                const std::time_t t = std::time(nullptr);
                std::strftime(name, sizeof(name), "screenshot-%Y%m%d-%H%M%S.png", std::localtime(&t));
                path = name;
            }
            SaveScreenshot(path, renderer->GetWidth(), renderer->GetHeight());
            if (!screenshotRequested) autoScreenshotPath.clear();
            screenshotRequested = false;
        }

        if (vsync != appliedVsync) {
            glfwSwapInterval(vsync ? 1 : 0);
            appliedVsync = vsync;
        }
        glfwSwapBuffers(window);
        lap(5);
        if (bench) {
            benchFrames++;
            if (now - benchLast >= 1.0) {
                const double n = benchFrames;
                std::printf("[bench] %.1f fps | network %.2f  update %.2f  raycast %.2f  render %.2f  gpu %.2f  ui %.2f  swap %.2f ms | %zu chunks, %zu meshed, %zu drawn\n",
                            n / (now - benchLast), benchSections[0] / n * 1e3, benchSections[1] / n * 1e3, benchSections[2] / n * 1e3,
                            benchSections[3] / n * 1e3, benchSections[6] / n * 1e3, benchSections[4] / n * 1e3, benchSections[5] / n * 1e3, world->GetLoadedChunkCount(),
                            world->GetMeshedChunkCount(), world->GetDrawnChunkCount());
                std::fflush(stdout);
                benchLast = now;
                benchFrames = 0;
                for (double& s : benchSections) s = 0.0;
            }
        }
    }
}

void Application::UpdateSky(float deltaTime, FrameParams& params) {
    // The sun goes round once a day, rising in the east (+x) at time 0.
    const float angle = client->GetTimeOfDay() * 6.2831853f;
    const float height = std::sin(angle);
    params.sunDirection = glm::normalize(glm::vec3(std::cos(angle), height, 0.35f));
    // Full daylight once the sun is a little up, dark once it's a little down.
    const float day = glm::smoothstep(-0.12f, 0.25f, height);
    // Warm sky while the sun is near the horizon.
    const float warmth = day * (1.0f - glm::smoothstep(0.0f, 0.4f, std::abs(height)));
    params.skyTint = glm::mix(glm::vec3(1.0f), glm::vec3(1.0f, 0.72f, 0.5f), warmth * 0.7f);

    // Rain dims the day and greys the sky; it doesn't fall where something is overhead
    // (eased, so walking under a roof doesn't cut it off in one frame).
    const float rain = client->GetRain();
    sheltered = false;
    const glm::ivec3 eye(glm::floor(camera.GetPosition()));
    for (int y = eye.y + 1; y <= eye.y + 48 && !sheltered; y++) {
        Block block;
        if (world->GetBlock(eye.x, y, eye.z, block) && !block.IsAir() && !block.IsPlant()) sheltered = true;
    }
    const float target = (sheltered || params.underwater) ? 0.0f : rain;
    const float ease = std::min(1.0f, deltaTime * 4.0f);
    rainHere += (target - rainHere) * ease;
    params.rain = rainHere;
    params.overcast = rain;
    params.sunBrightness = day * (1.0f - 0.45f * rain);
}

void Application::UpdateNetwork() {
    client->Poll();

    // Joined: start at the spawn point the server picked, and drop onto the terrain
    // there once it has arrived.
    if (!joined && client->IsWelcomed()) {
        joined = true;
        player.Teleport(client->GetSpawn());
        player.ResetSpawn();
        // Rejoining a saved world: back where we left, facing the same way.
        if (client->GetSpawnFlags() & net::kWelcomeResume) {
            player.ResumeAt(client->GetSpawn());
            camera.SetLook(client->GetSpawnYaw(), client->GetSpawnPitch());
        }
    }
    if (auto correction = client->TakeCorrection()) {
        player.Teleport(*correction);
        // Corrected into terrain (e.g. back to the spawn point): climb out onto it.
        if (!player.IsFlying() && Collision::BoxBlocked(*world, player.GetBounds())) player.ResetSpawn();
    }
}

void Application::DrawCrosshair() {
    // Only while aiming: not with the cursor free, the inventory open or typing.
    if (!captureCursor || inventoryOpen || chatOpen || !client->IsWelcomed()) return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 c(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
    constexpr float kArm = 9.0f, kGap = 0.0f;
    // A dark outline under a white cross, so it shows on sky and snow alike.
    for (const auto& [color, width] : { std::pair<ImU32, float>{ IM_COL32(0, 0, 0, 170), 4.0f }, { IM_COL32(255, 255, 255, 230), 2.0f } }) {
        draw->AddLine(ImVec2(c.x - kArm, c.y), ImVec2(c.x - kGap, c.y), color, width);
        draw->AddLine(ImVec2(c.x + kGap, c.y), ImVec2(c.x + kArm, c.y), color, width);
        draw->AddLine(ImVec2(c.x, c.y - kArm), ImVec2(c.x, c.y - kGap), color, width);
        draw->AddLine(ImVec2(c.x, c.y + kGap), ImVec2(c.x, c.y + kArm), color, width);
    }
}

void Application::DrawHealth() {
    if (!client->IsWelcomed()) return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // A red flash when hurt, fading over half a second.
    if (client->IsCreative()) return; // nothing hurts in creative

    // Running out of air darkens the edges of the screen (blue under water), more as
    // it goes, with a slow pulse once it's gone.
    const int air = client->GetAir(), maxAir = std::max(1, client->GetMaxAir());
    const Breathing::Cause cause = client->GetAirCause();
    const float airLeft = static_cast<float>(air) / maxAir;
    if (cause != Breathing::Cause::None && airLeft < 0.6f) {
        float strength = (0.6f - airLeft) / 0.6f;
        if (air == 0) strength = 0.85f + 0.15f * std::sin(static_cast<float>(Client::Now()) * 4.0f);
        const ImVec2 lo = viewport->WorkPos, hi(lo.x + viewport->WorkSize.x, lo.y + viewport->WorkSize.y);
        const bool water = cause == Breathing::Cause::Water;
        const int r = water ? 0 : 10, g = water ? 20 : 8, b = water ? 60 : 12;
        const ImU32 edge = IM_COL32(r, g, b, static_cast<int>(230 * strength)), clear = IM_COL32(r, g, b, 0);
        const float band = std::min(viewport->WorkSize.x, viewport->WorkSize.y) * (0.15f + 0.3f * strength);
        draw->AddRectFilledMultiColor(lo, ImVec2(hi.x, lo.y + band), edge, edge, clear, clear);
        draw->AddRectFilledMultiColor(ImVec2(lo.x, hi.y - band), hi, clear, clear, edge, edge);
        draw->AddRectFilledMultiColor(lo, ImVec2(lo.x + band, hi.y), edge, clear, clear, edge);
        draw->AddRectFilledMultiColor(ImVec2(hi.x - band, lo.y), hi, clear, edge, edge, clear);
    }

    const float sinceHurt = static_cast<float>(Client::Now() - client->GetLastHurt());
    if (sinceHurt < 0.5f) {
        const int alpha = static_cast<int>(110.0f * (1.0f - sinceHurt / 0.5f));
        draw->AddRectFilled(viewport->WorkPos, ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y),
                            IM_COL32(200, 20, 20, alpha));
    }
    if (player.IsFlying()) return; // no-clip: nothing can hurt you

    // Hearts above the hotbar's left end: full, half or empty.
    constexpr float kSize = 9.0f, kGap = 4.0f;
    const int hearts = (client->GetMaxHealth() + 1) / 2, health = client->GetHealth();
    const float hotbarWidth = kHotbarSlots * 52.0f + (kHotbarSlots - 1) * 4.0f + 16.0f;
    const float x0 = viewport->WorkPos.x + (viewport->WorkSize.x - hotbarWidth) * 0.5f + 6.0f;
    const float y = viewport->WorkPos.y + viewport->WorkSize.y - 12.0f - 98.0f;
    auto heart = [&](float cx, float cy, ImU32 color) {
        // Two lobes and a point.
        draw->AddCircleFilled(ImVec2(cx - kSize * 0.45f, cy), kSize * 0.55f, color);
        draw->AddCircleFilled(ImVec2(cx + kSize * 0.45f, cy), kSize * 0.55f, color);
        draw->AddTriangleFilled(ImVec2(cx - kSize, cy + kSize * 0.15f), ImVec2(cx + kSize, cy + kSize * 0.15f), ImVec2(cx, cy + kSize * 1.1f), color);
    };
    const bool blink = sinceHurt < 0.6f && static_cast<int>(sinceHurt * 10.0f) % 2 == 0;
    for (int i = 0; i < hearts; i++) {
        const float cx = x0 + kSize + i * (2.0f * kSize + kGap);
        heart(cx + 1.5f, y + 1.5f, IM_COL32(0, 0, 0, 160));       // shadow
        heart(cx, y, IM_COL32(60, 50, 50, 220));                    // empty
        const int left = health - 2 * i;
        const ImU32 red = blink ? IM_COL32(255, 200, 200, 255) : IM_COL32(220, 30, 40, 255);
        if (left >= 2) heart(cx, y, red);
        else if (left == 1) {
            draw->PushClipRect(ImVec2(cx - kSize * 1.2f, y - kSize), ImVec2(cx, y + kSize * 1.2f), true); // left half
            heart(cx, y, red);
            draw->PopClipRect();
        }
    }

    // Air bubbles above the hotbar's right end, right to left, while anything takes
    // your breath or it's coming back; a bubble is two units of air.
    if (cause != Breathing::Cause::None || air < maxAir) {
        const int bubbles = (maxAir + 1) / 2;
        const float right = viewport->WorkPos.x + (viewport->WorkSize.x + hotbarWidth) * 0.5f - 6.0f;
        for (int i = 0; i < bubbles; i++) {
            const float cx = right - kSize - i * (2.0f * kSize + kGap);
            const int left = air - 2 * i;
            if (left <= 0) {
                draw->AddCircleFilled(ImVec2(cx, y + 2.0f), kSize * 0.75f, IM_COL32(20, 30, 45, 150)); // gone
                draw->AddCircle(ImVec2(cx, y + 2.0f), kSize * 0.75f, IM_COL32(140, 180, 220, 170), 0, 1.5f);
                continue;
            }
            const float radius = kSize * (left >= 2 ? 0.8f : 0.5f); // half a bubble is a small one
            draw->AddCircleFilled(ImVec2(cx + 1.5f, y + 3.5f), radius, IM_COL32(0, 0, 0, 140));
            draw->AddCircleFilled(ImVec2(cx, y + 2.0f), radius, IM_COL32(90, 170, 255, 235));
            draw->AddCircle(ImVec2(cx, y + 2.0f), radius, IM_COL32(220, 240, 255, 255), 0, 1.5f);
            draw->AddCircleFilled(ImVec2(cx - radius * 0.35f, y + 2.0f - radius * 0.35f), radius * 0.25f, IM_COL32(255, 255, 255, 230)); // shine
        }
        if (cause != Breathing::Cause::None) {
            const char* label = CauseName(cause);
            const ImVec2 size = ImGui::CalcTextSize(label);
            const ImVec2 at(right - size.x, y - kSize - size.y - 4.0f);
            draw->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 220), label);
            draw->AddText(at, air == 0 ? IM_COL32(255, 120, 110, 255) : IM_COL32(200, 230, 255, 255), label);
        }
    }
}

void Application::OpenChat(char openedWith) {
    chatOpen = true;
    chatOpenedWith = openedWith;
    chatOpenFrames = 0;
    std::snprintf(chatInput, sizeof chatInput, "%s", openedWith == '/' ? "/" : "");
}

void Application::DrawChat() {
    constexpr double kShowSeconds = 10.0, kFadeSeconds = 2.0;
    constexpr size_t kLinesClosed = 8, kLinesOpen = 16;
    const auto& lines = client->GetChat();
    const double now = Client::Now();

    // Which lines to show: while typing, the last few; otherwise only recent ones, fading.
    size_t first = lines.size();
    const size_t limit = chatOpen ? kLinesOpen : kLinesClosed;
    while (first > 0 && lines.size() - first < limit && (chatOpen || now - lines[first - 1].time < kShowSeconds)) first--;
    if (first == lines.size() && !chatOpen) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(560.0f, viewport->WorkSize.x - 24.0f);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 12.0f, viewport->WorkPos.y + viewport->WorkSize.y - 100.0f), ImGuiCond_Always,
                            ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    ImGui::SetNextWindowBgAlpha(chatOpen ? 0.7f : 0.55f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f)); // black, so text reads over bright terrain
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    const bool visible = ImGui::Begin("Chat", nullptr, flags);
    ImGui::PopStyleColor(2);
    if (!visible) {
        ImGui::End();
        return;
    }
    ImGui::PushTextWrapPos(width - 16.0f);
    for (size_t i = first; i < lines.size(); i++) {
        const auto& line = lines[i];
        ImVec4 color(1.0f, 1.0f, 1.0f, 1.0f);
        switch (line.kind) {
        case net::ChatMessage::Kind::Info: color = ImVec4(1.0f, 0.88f, 0.45f, 1.0f); break;
        case net::ChatMessage::Kind::Error: color = ImVec4(1.0f, 0.5f, 0.45f, 1.0f); break;
        case net::ChatMessage::Kind::Private: color = ImVec4(0.82f, 0.65f, 1.0f, 1.0f); break;
        default: break;
        }
        if (!chatOpen) color.w = static_cast<float>(std::clamp((kShowSeconds - (now - line.time)) / kFadeSeconds, 0.0, 1.0));
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(line.text.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();

    if (chatOpen) {
        if (chatOpenFrames < 2) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0f);
        const bool send = ImGui::InputText("##chat", chatInput, sizeof chatInput, ImGuiInputTextFlags_EnterReturnsTrue);
        // The key that opened the chat may also arrive as a typed character: drop the
        // 't', and keep a single '/'.
        if (chatOpenFrames < 3) {
            if (chatOpenedWith == 't' && (std::strcmp(chatInput, "t") == 0 || std::strcmp(chatInput, "T") == 0)) chatInput[0] = 0;
            if (chatOpenedWith == '/' && std::strcmp(chatInput, "//") == 0) chatInput[1] = 0;
        }
        chatOpenFrames++;
        if (send) {
            client->SendChat(chatInput);
            chatOpen = false;
        }
    }
    ImGui::End();
}

void Application::DrawStatus() {
    std::string text;
    if (!client->IsConnected() && !client->GetDisconnectReason().empty()) text = "Disconnected: " + client->GetDisconnectReason();
    else if (!client->IsWelcomed()) text = "Connecting to " + serverLabel + "...";
    else if (!player.IsSpawned() && !player.IsFlying()) text = "Loading terrain...";
    if (text.empty()) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + 40.0f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("Status", nullptr, flags)) ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

void Application::HandleContinuousInput(float deltaTime) {
    auto down = [&](int key) { return glfwGetKey(window, key) == GLFW_PRESS; };
    if (!joined) return;

    // The player drops onto the terrain once the chunks under it have loaded.
    if (!player.IsSpawned() && !player.IsFlying()) player.Spawn(*world);

    // No-clip is the server's to switch (/noclip): follow it.
    const bool noclip = client->GetAbilities() & net::kAbilityNoclip;
    if (noclip != player.IsFlying()) player.SetFlying(*world, noclip);

    PlayerInput input;
    if (!chatOpen && !inventoryOpen) {
    input.move = glm::vec2((down(GLFW_KEY_D) ? 1.0f : 0.0f) - (down(GLFW_KEY_A) ? 1.0f : 0.0f),
                           (down(GLFW_KEY_W) ? 1.0f : 0.0f) - (down(GLFW_KEY_S) ? 1.0f : 0.0f));
    input.jump = down(GLFW_KEY_SPACE) || (player.IsFlying() && down(GLFW_KEY_E));
    input.descend = down(GLFW_KEY_Q) || down(GLFW_KEY_LEFT_CONTROL);
    // Short of breath (thin or stale air, half the air gone): too winded to sprint.
    const Breathing::Cause cause = client->GetAirCause();
    const bool winded = (cause == Breathing::Cause::ThinAir || cause == Breathing::Cause::StaleAir) && client->GetAir() < Breathing::kShortOfBreath;
    input.sprint = down(GLFW_KEY_LEFT_SHIFT) && !winded;
    }
    input.yaw = camera.GetYaw();
    input.lookDirection = camera.GetDirection();
    player.Update(*world, input, deltaTime);

    // Our position is ours to predict; the server checks it loosely and corrects us if
    // it disagrees. Nothing is sent until we've landed on the terrain.
    if (player.IsSpawned() || player.IsFlying()) {
        uint8_t flags = 0;
        if (player.IsFlying()) flags |= net::kFlagFlying;
        if (player.IsOnGround()) flags |= net::kFlagOnGround;
        if (player.IsSwimming()) flags |= net::kFlagSwimming;
        client->SendPlayerState(player.GetFeetPosition(), player.GetVelocity(), camera.GetYaw(), camera.GetDirectionAngles().x, flags);
    }

    // Laser: carve a tunnel along the view ray while L is held.
    if (captureCursor && !chatOpen && (client->GetAbilities() & net::kAbilityOperator) && glfwGetKey(window, GLFW_KEY_L) == GLFW_PRESS) {
        client->Carve(TraceBlocks(*world, camera.GetPosition(), camera.GetDirection(), kLaserRange), kLaserRadius);
    }
}

void Application::DrawHotbar() {
    constexpr int kSlots = kHotbarSlots;
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
        // Name of the selected block, centred above the slots, with the height it's
        // placed at (V) and, in survival, what that costs.
        const BlockType selected = SelectedBlock();
        const bool survival = !client->IsCreative();
        std::string label = BlockName(selected);
        const int height = Block::CanShape(selected) ? placeHeight : 8;
        if (Block::CanShape(selected)) label += std::string(" - ") + HeightName(height) + (height < 8 ? " height" : " block") + " (V)";
        const bool affordable = client->CanAfford(selected, height);
        if (survival) {
            if (Block::Placed(selected, height).IsLiquid()) label += " - can't be placed in survival";
            else label += " - costs " + FormatBlocks(static_cast<uint32_t>(Block::Placed(selected, height).ResourceUnits())) + ", have " +
                          FormatBlocks(client->GetResources(selected));
        }
        const float nameWidth = ImGui::CalcTextSize(label.c_str()).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (barWidth - nameWidth) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, affordable ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(1.0f, 0.55f, 0.5f, 1.0f));
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopStyleColor();

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
            atlas.GetTileUV(IconTile(hotbar[i]), uv0, uv1);
            const uint32_t held = client->GetResources(hotbar[i]);
            const bool empty = survival && held == 0;
            draw->AddImage(atlasTexture, ImVec2(slotMin.x + kPad, slotMin.y + kPad), ImVec2(slotMax.x - kPad, slotMax.y - kPad),
                           ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv1.y), empty ? IM_COL32(255, 255, 255, 70) : IM_COL32_WHITE);
            // Survival: how many we have, in the bottom right.
            if (survival && held > 0) {
                const std::string count = FormatBlocks(held);
                const ImVec2 size = ImGui::CalcTextSize(count.c_str());
                const ImVec2 at(slotMax.x - size.x - 4.0f, slotMax.y - size.y - 2.0f);
                draw->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 230), count.c_str());
                draw->AddText(at, IM_COL32(255, 255, 255, 255), count.c_str());
            }
            draw->AddRect(slotMin, slotMax, selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 60), 4.0f, 0, selected ? 3.0f : 1.0f);
            // Number key in the corner (1-9, then 0; later slots are picked with the wheel).
            char key[2] = { static_cast<char>(i < 9 ? '1' + i : i == 9 ? '0' : 0), 0 };
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
        {
            const float t = client->GetTimeOfDay();
            const int minutes = static_cast<int>((t * 24.0f + 6.0f) * 60.0f) % (24 * 60); // sunrise at 06:00
            ImGui::Text("Time %02d:%02d (N skips to dawn / dusk), rain %.0f%%%s", minutes / 60, minutes % 60, client->GetRain() * 100.0f,
                        sheltered ? ", sheltered" : "");
        }
        ImGui::Text("Placing: %s", BlockName(SelectedBlock()));
        ImGui::Text("Position %.3f, %.3f, %.3f", pos.x, pos.y, pos.z);
        if (!(client->GetSpawnFlags() & net::kWelcomeEmptyWorld)) {
            // The world generator is deterministic, so the biome can be worked out here.
            static const TerrainGenerator climate;
            const int bx = static_cast<int>(std::floor(pos.x)), bz = static_cast<int>(std::floor(pos.z));
            const int row = climate.ColumnAt(bx, bz).surfaceRow;
            const auto c = climate.ClimateAt(bx, bz, row);
            ImGui::Text("Biome: %s (temperature %.2f, humidity %.2f)", BiomeName(TerrainGenerator::Classify(c)), c.temperature, c.humidity);
        }
        const glm::vec3 velocity = player.GetVelocity();
        ImGui::Text("%s%s, velocity %.2f, %.2f, %.2f", player.IsFlying() ? "Flying" : player.IsSwimming() ? "Swimming" : "Walking",
                    player.IsFlying() || player.IsSwimming() ? "" : player.IsOnGround() ? " (on ground)" : " (airborne)",
                    velocity.x, velocity.y, velocity.z);
        ImGui::Text("Angles %.3f, %.3f", angles.x, angles.y);
        ImGui::Text("Frame %.3f ms (%.1f FPS)", deltaTime * 1000.0f, deltaTime > 0.0f ? 1.0f / deltaTime : 0.0f);
        ImGui::PlotLines("Frame Time (ms)", frameTimes.data(), kFrameHistory, frameTimeIndex);
        ImGui::Text("Chunks: %zu received, %zu tracked, %d alive", world->GetLoadedChunkCount(), world->GetChunkCount(), Chunk::GetLiveCount());
        const Client::Stats net = client->GetStats();
        ImGui::Text("Server: %s, player %u, ping %u ms", serverLabel.c_str(), client->GetPlayerId(), net.roundTripMs);
        ImGui::Text("Received %.1f MB: %u chunks, %u cell updates, %u light updates", net.bytesReceived / (1024.0 * 1024.0),
                    net.chunksReceived, net.deltasReceived, net.lightUpdates);
        ImGui::Text("Other players: %zu", client->GetRemotePlayers().size());

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

    // While typing, keys belong to the chat line (ImGui gets them too); Esc closes it.
    if (chatOpen) {
        if (key == GLFW_KEY_ESCAPE) chatOpen = false;
        return;
    }
    // The inventory takes I and Esc (to close) and the number keys (to pick a slot).
    if (inventoryOpen) {
        if (key == GLFW_KEY_I || key == GLFW_KEY_ESCAPE) SetInventoryOpen(false);
        else if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) SelectSlot(key - GLFW_KEY_1);
        else if (key == GLFW_KEY_0) SelectSlot(9);
        return;
    }

    switch (key) {
    case GLFW_KEY_I:
        SetInventoryOpen(true);
        break;
    case GLFW_KEY_V: {
        // Placement height: full, 3/4, 1/2, 1/4 (Shift+V goes back up).
        const bool shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        placeHeight = shift ? (placeHeight >= 8 ? 2 : placeHeight + 2) : (placeHeight <= 2 ? 8 : placeHeight - 2);
        break;
    }
    case GLFW_KEY_C:
        if (captureCursor) ChiselTarget(glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
        break;
    case GLFW_KEY_T:
        OpenChat('t');
        break;
    case GLFW_KEY_SLASH:
        OpenChat('/');
        break;
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER:
        OpenChat(0);
        break;
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
    case GLFW_KEY_F12:
        screenshotRequested = true;
        break;
    case GLFW_KEY_N:
        client->SkipTime();
        break;
    default:
        if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) SelectSlot(key - GLFW_KEY_1);
        else if (key == GLFW_KEY_0) SelectSlot(9);
        break;
    }
}

BlockType Application::SelectedBlock() const {
    return hotbar[selectedSlot];
}

void Application::SelectSlot(int slot) {
    selectedSlot = ((slot % kHotbarSlots) + kHotbarSlots) % kHotbarSlots;
}

void Application::LoadHotbar() {
    std::ifstream file(kHotbarFile);
    for (int i = 0, type; i < kHotbarSlots && file >> type; i++)
        if (std::find(std::begin(kInventoryBlocks), std::end(kInventoryBlocks), static_cast<BlockType>(type)) != std::end(kInventoryBlocks))
            hotbar[i] = static_cast<BlockType>(type);
}

void Application::SaveHotbar() const {
    std::ofstream file(kHotbarFile, std::ios::trunc);
    for (BlockType type : hotbar) file << static_cast<int>(type) << ' ';
    file << "\n";
}

void Application::SetInventoryOpen(bool open) {
    if (open == inventoryOpen) return;
    inventoryOpen = open;
    // The cursor is needed to click blocks; give it back to the game on closing.
    if (open) cursorBeforeInventory = captureCursor;
    captureCursor = open ? false : cursorBeforeInventory;
    glfwSetInputMode(window, GLFW_CURSOR, captureCursor ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    camera.ResetLook();
}

void Application::DrawInventory() {
    if (!inventoryOpen) return;
    constexpr int kColumns = 7;
    constexpr float kIcon = 44.0f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize;
    bool open = true;
    if (ImGui::Begin("Blocks", &open, flags)) {
        const char key = selectedSlot < 9 ? static_cast<char>('1' + selectedSlot) : '0';
        const bool survival = !client->IsCreative();
        ImGui::Text("Click a block to put it in hotbar slot %c.", key);
        ImGui::TextDisabled("Pick the slot with 1-9, 0 or by clicking it below. I or Esc closes.");
        if (survival) ImGui::TextDisabled("Survival: these are the blocks you've collected. Break blocks to get more.");
        ImGui::Spacing();
        const TextureAtlas& atlas = renderer->GetAtlas();
        const ImTextureID texture = static_cast<ImTextureID>(static_cast<uintptr_t>(atlas.GetTexture()));
        int column = 0;
        for (BlockType type : kInventoryBlocks) {
            const uint32_t held = client->GetResources(type);
            if (survival && held == 0) continue; // survival: only what you have
            if (column++ % kColumns != 0) ImGui::SameLine();
            const ImVec2 corner = ImGui::GetCursorScreenPos();
            glm::vec2 uv0, uv1;
            atlas.GetTileUV(IconTile(type), uv0, uv1);
            ImGui::PushID(static_cast<int>(type));
            const bool inSlot = hotbar[selectedSlot] == type;
            if (inSlot) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.35f));
            if (ImGui::ImageButton("block", texture, ImVec2(kIcon, kIcon), ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv1.y))) {
                hotbar[selectedSlot] = type;
                SaveHotbar();
            }
            if (inSlot) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                if (survival) ImGui::SetTooltip("%s: %s", BlockName(type), FormatBlocks(held).c_str());
                else ImGui::SetTooltip("%s", BlockName(type));
            }
            if (survival) {
                const std::string count = FormatBlocks(held);
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const ImVec2 size = ImGui::CalcTextSize(count.c_str());
                const ImVec2 at(corner.x + kIcon + 4.0f - size.x, corner.y + kIcon + 4.0f - size.y);
                draw->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 230), count.c_str());
                draw->AddText(at, IM_COL32_WHITE, count.c_str());
            }
            ImGui::PopID();
        }
        if (survival && column == 0) ImGui::TextUnformatted("Nothing yet: break some blocks.");
    }
    ImGui::End();
    if (!open) SetInventoryOpen(false);
}

void Application::ChiselTarget(bool raise) {
    auto hit = Raycast(*world, camera.GetPosition(), camera.GetDirection(), kReach);
    if (!hit) return;
    // Where the view ray enters the block's cube decides the quarter (in x and z).
    const glm::vec3 origin = camera.GetPosition(), dir = camera.GetDirection();
    const glm::vec3 lo(hit->block), hi = lo + glm::vec3(1.0f);
    float tNear = 0.0f, tFar = 1e9f;
    for (int a = 0; a < 3; a++) {
        if (std::abs(dir[a]) < 1e-6f) continue;
        float t0 = (lo[a] - origin[a]) / dir[a], t1 = (hi[a] - origin[a]) / dir[a];
        if (t0 > t1) std::swap(t0, t1);
        tNear = std::max(tNear, t0);
        tFar = std::min(tFar, t1);
    }
    const glm::vec3 p = origin + dir * std::min(tNear, tFar) - lo;
    const bool highX = p.x >= 0.5f, highZ = p.z >= 0.5f;
    const int edge = highX ? (highZ ? 3 : 1) : (highZ ? 0 : 2); // EdgeData order
    client->Chisel(hit->block, edge, raise);
}

void Application::OnMouseButton(int button, int action) {
    // With the cursor released, clicks belong to the UI.
    if (action != GLFW_PRESS || !captureCursor || chatOpen) return;

    auto hit = Raycast(*world, camera.GetPosition(), camera.GetDirection(), kReach);
    if (!hit) return;

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        client->BreakBlock(hit->block);
    }
    else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        const glm::ivec3 place = hit->block + hit->normal;
        // Don't build a solid block inside yourself when walking (water, sprites and
        // pressure plates are fine).
        const BlockType placeBlock = SelectedBlock();
        const int height = Block::CanShape(placeBlock) ? placeHeight : 8;
        if (!client->CanAfford(placeBlock, height)) return;
        const Block placed = Block::Placed(placeBlock, height);
        if (!player.IsFlying() && (placed.IsFullBlock() || Block::CanShape(placeBlock))) {
            const Collision::AABB body = player.GetBounds();
            const glm::vec3 lo(place), hi = lo + glm::vec3(1.0f);
            if (glm::all(glm::lessThan(lo, body.max)) && glm::all(glm::greaterThan(hi, body.min))) return;
        }
        // Logs lie along the axis of the face they're placed against.
        const int faceAxis = hit->normal.x != 0 ? 0 : hit->normal.z != 0 ? 2 : 1;
        client->PlaceBlock(place, placeBlock, height, faceAxis);
    }
}

void Application::OnCursorMove(double x, double y) {
    if (captureCursor && !ignoreMouse) camera.UpdateLook(x, y);
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
