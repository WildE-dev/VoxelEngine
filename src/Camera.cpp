#include "Camera.h"

void Camera::UpdateLook(double xPos, double yPos) {
    if (firstMouse)
    {
        lastX = xPos;
        lastY = yPos;
        firstMouse = false;
    }

    float xoffset = (float)(xPos - lastX);
    float yoffset = (float)(lastY - yPos);
    lastX = xPos;
    lastY = yPos;

    float sensitivity = 0.1f;
    xoffset *= sensitivity;
    yoffset *= sensitivity;

    yaw += xoffset;
    pitch += yoffset;

    if (pitch > 89.0f)
        pitch = 89.0f;
    if (pitch < -89.0f)
        pitch = -89.0f;

    glm::vec3 direction = glm::vec3();
    direction.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    direction.y = sin(glm::radians(pitch));
    direction.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
    cameraFront = glm::normalize(direction);
}

void Camera::UpdateMove(GLFWwindow* window, double deltaTime) {
    float cameraSpeed = 10.0f * (float)deltaTime;

    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
        cameraSpeed *= 2;

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        Camera::cameraPos += cameraSpeed * Camera::cameraFront;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        Camera::cameraPos -= cameraSpeed * Camera::cameraFront;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        Camera::cameraPos -= glm::normalize(glm::cross(Camera::cameraFront, Camera::cameraUp)) * cameraSpeed;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        Camera::cameraPos += glm::normalize(glm::cross(Camera::cameraFront, Camera::cameraUp)) * cameraSpeed;
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS)
        Camera::cameraPos -= Camera::cameraUp * cameraSpeed;
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)
        Camera::cameraPos += Camera::cameraUp * cameraSpeed;
}

glm::mat4 Camera::GetViewMatrix() const {
    return glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);
}

glm::mat4 Camera::GetProjectionMatrix(float frameWidth, float frameHeight) const {
    return glm::perspective(glm::radians(45.0f), frameWidth / frameHeight, 0.1f, 1000.0f);
}

glm::vec3 Camera::GetPosition() {
    return cameraPos;
}

glm::vec3 Camera::GetDirection() {
    return cameraFront;
}

glm::vec2 Camera::GetDirectionAngles() const
{
    return glm::vec2(pitch, yaw);
}