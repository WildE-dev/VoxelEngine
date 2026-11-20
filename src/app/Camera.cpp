#include "app/Camera.h"

#include <algorithm>

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace {
constexpr float kMouseSensitivity = 0.1f;
constexpr float kFovDegrees = 45.0f;
}

void Camera::UpdateLook(double xPos, double yPos) {
    if (firstMouse) {
        lastX = xPos;
        lastY = yPos;
        firstMouse = false;
    }

    float xOffset = static_cast<float>(xPos - lastX) * kMouseSensitivity;
    float yOffset = static_cast<float>(lastY - yPos) * kMouseSensitivity;
    lastX = xPos;
    lastY = yPos;

    yaw += xOffset;
    pitch = glm::clamp(pitch + yOffset, -89.0f, 89.0f);
    UpdateFront();
}

void Camera::SetYaw(float degrees) {
    yaw = degrees;
    UpdateFront();
}

void Camera::SetLook(float yawDegrees, float pitchDegrees) {
    yaw = yawDegrees;
    pitch = std::clamp(pitchDegrees, -89.0f, 89.0f);
    UpdateFront();
}

void Camera::UpdateFront() {
    glm::vec3 direction;
    direction.x = std::cos(glm::radians(yaw)) * std::cos(glm::radians(pitch));
    direction.y = std::sin(glm::radians(pitch));
    direction.z = std::sin(glm::radians(yaw)) * std::cos(glm::radians(pitch));
    front = glm::normalize(direction);
}

glm::mat4 Camera::GetViewMatrix() const {
    return glm::lookAt(position, position + front, up);
}

glm::mat4 Camera::GetProjectionMatrix(float aspectRatio) const {
    return glm::perspective(glm::radians(kFovDegrees), aspectRatio, 0.1f, 1000.0f);
}
