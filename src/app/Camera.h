#pragma once

#include <glm/glm.hpp>

struct GLFWwindow;

// Free-fly first person camera.
class Camera
{
public:
    // Applies a mouse movement (in screen coordinates) to yaw/pitch.
    void UpdateLook(double xPos, double yPos);
    // Call when the cursor is released so the next look update doesn't jump.
    void ResetLook() { firstMouse = true; }
    // WASD + QE movement, Shift to go faster.
    void UpdateMove(GLFWwindow* window, float deltaTime);

    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix(float aspectRatio) const;

    glm::vec3 GetPosition() const { return position; }
    glm::vec3 GetDirection() const { return front; }
    glm::vec2 GetDirectionAngles() const { return { pitch, yaw }; }

private:
    glm::vec3 position{7.0f, 7.0f, 7.0f};
    glm::vec3 front{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};

    bool firstMouse = true;
    double lastX = 0.0, lastY = 0.0;
    float pitch = 0.0f, yaw = -90.0f;
};
