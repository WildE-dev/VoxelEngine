#pragma once
#include "Camera.h"
#include <GLFW/glfw3.h>

class DebugMenu {
public:
	DebugMenu();
	~DebugMenu();

	void Initialize(GLFWwindow* window, const char* glsl_version);
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