#include "DebugMenu.h"
#include "Chunk.h"

DebugMenu::DebugMenu() {
    for (int i = 0; i < 100; ++i) {
        frameTimes[i] = 0;
    }
}

DebugMenu::~DebugMenu() {
	Cleanup();
}

void DebugMenu::Initialize(SDL_Window* window, SDL_GLContext context, const char* glsl_version) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForOpenGL(window, context);
    ImGui_ImplOpenGL3_Init(glsl_version);
}

void DebugMenu::ProcessEvent(SDL_Event* event) {
    ImGui_ImplSDL3_ProcessEvent(event);
}

void DebugMenu::Render(Camera& camera, float deltaTime) {
    float frameTime = 1000.0f * deltaTime;
    float fps = 1.0f / deltaTime;

    frameTimer += deltaTime;

    if (frameTimer > 0.01f) {
        frameTimes[i] = frameTime;
        i++;
        if (i >= 100) i = 0;
        frameTimer = 0.0f;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
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
        }

        ImGui::End();
    }

    ImGui::Render();

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void DebugMenu::Cleanup() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void DebugMenu::ToggleVSync() {
    vsync = !vsync;
}

void DebugMenu::ToggleDebug() {
    debug = !debug;
}

void DebugMenu::ToggleWireframe() {
    wireframe = !wireframe;
}

bool DebugMenu::GetVSync()
{
    return vsync;
}

bool DebugMenu::GetDebug()
{
    return debug;
}

bool DebugMenu::GetWireframe()
{
    return wireframe;
}
