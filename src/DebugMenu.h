#pragma once
#include "Camera.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>

#include <SDL3/SDL.h>

class DebugMenu {
public:
	DebugMenu();
	~DebugMenu();

	void Initialize(SDL_Window* window, SDL_GLContext context, const char* glsl_version);
	void Render(Camera& camera, float deltaTime);
	void Cleanup();

	void ToggleVSync();
	void ToggleDebug();
	void ToggleWireframe();

	bool GetVSync();
	bool GetDebug();
	bool GetWireframe();
private:
	bool vsync = true;
	bool wireframe = false;
	bool debug = false;

	double lastFrame = 0.0f;
	float frameTimes[100];
	int i = 0;
	double frameTimer = 0.0f;
};