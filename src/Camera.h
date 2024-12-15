#pragma once
#include <glm.hpp>
#include <gtc/matrix_transform.hpp>

class Camera
{
public:
	//Camera();
	void UpdateLook(float xMotion, float yMotion);
	void UpdateMove(const bool* keys, double deltaTime);
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

