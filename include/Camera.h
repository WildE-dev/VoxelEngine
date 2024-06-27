#pragma once
#include <glm.hpp>
#include <GLFW/glfw3.h>
#include <gtc/matrix_transform.hpp>

class Camera
{
public:
	bool firstMouse = true;

	//Camera();
	void UpdateLook(double xPos, double yPos);
	void UpdateMove(GLFWwindow* window, double deltaTime);
	glm::mat4 GetViewMatrix() const;
	glm::mat4 GetProjectionMatrix(float frameWidth, float frameHeight) const;
	glm::vec3 GetPosition() const;
	glm::vec3 GetDirection() const;
	glm::vec2 GetDirectionAngles() const;
private:
	glm::vec3 cameraPos = glm::vec3(7.0f, 7.0f, 7.0f);
	glm::vec3 cameraFront = glm::vec3(0.0f, 0.0f, -1.0f);
	glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
	double lastX = 400, lastY = 300;
	float pitch = 0.0f, yaw = -90.0f;
};

