#include "Camera.h"

#include <SDL3/SDL.h>

void Camera::UpdateLook(float xMotion, float yMotion) {
    float sensitivity = 0.1f;
    float xoffset = xMotion * sensitivity;
    float yoffset = yMotion * -sensitivity;

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

void Camera::UpdateMove(const bool* keys, double deltaTime) {
    float cameraSpeed = 10.0f * (float)deltaTime;

    if (keys[SDL_SCANCODE_LSHIFT])
        cameraSpeed *= 2;

    if (keys[SDL_SCANCODE_W])
        Camera::cameraPos += cameraSpeed * Camera::cameraFront;
    if (keys[SDL_SCANCODE_S])
        Camera::cameraPos -= cameraSpeed * Camera::cameraFront;
    if (keys[SDL_SCANCODE_A])
        Camera::cameraPos -= glm::normalize(glm::cross(Camera::cameraFront, Camera::cameraUp)) * cameraSpeed;
    if (keys[SDL_SCANCODE_D])
        Camera::cameraPos += glm::normalize(glm::cross(Camera::cameraFront, Camera::cameraUp)) * cameraSpeed;
    if (keys[SDL_SCANCODE_Q])
        Camera::cameraPos -= Camera::cameraUp * cameraSpeed;
    if (keys[SDL_SCANCODE_E])
        Camera::cameraPos += Camera::cameraUp * cameraSpeed;
}

glm::mat4 Camera::GetViewMatrix() const {
    return glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);
}

glm::mat4 Camera::GetProjectionMatrix(float frameWidth, float frameHeight) const {
    return glm::perspective(glm::radians(45.0f), frameWidth / frameHeight, 0.1f, 1000.0f);
}

glm::vec3 Camera::GetPosition() const {
    return cameraPos;
}

glm::vec3 Camera::GetDirection() const {
    return cameraFront;
}

glm::vec2 Camera::GetDirectionAngles() const
{
    return glm::vec2(pitch, yaw);
}