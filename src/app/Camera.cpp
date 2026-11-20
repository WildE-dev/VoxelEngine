#include "app/Camera.h"

#include <cmath>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

namespace {
constexpr float kMouseSensitivity = 0.1f;
constexpr float kMoveSpeed = 10.0f;
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

    glm::vec3 direction;
    direction.x = std::cos(glm::radians(yaw)) * std::cos(glm::radians(pitch));
    direction.y = std::sin(glm::radians(pitch));
    direction.z = std::sin(glm::radians(yaw)) * std::cos(glm::radians(pitch));
    front = glm::normalize(direction);
}

void Camera::UpdateMove(GLFWwindow* window, float deltaTime) {
    float speed = kMoveSpeed * deltaTime;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
        speed *= 2.0f;

    const glm::vec3 right = glm::normalize(glm::cross(front, up));

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) position += front * speed;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) position -= front * speed;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) position -= right * speed;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) position += right * speed;
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) position -= up * speed;
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) position += up * speed;
}

glm::mat4 Camera::GetViewMatrix() const {
    return glm::lookAt(position, position + front, up);
}

glm::mat4 Camera::GetProjectionMatrix(float aspectRatio) const {
    return glm::perspective(glm::radians(kFovDegrees), aspectRatio, 0.1f, 1000.0f);
}
