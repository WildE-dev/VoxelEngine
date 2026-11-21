#pragma once

#include <glm/glm.hpp>

// First person view: orientation from the mouse, position set by the Player.
class Camera
{
public:
    // Applies a mouse movement (in screen coordinates) to yaw/pitch.
    void UpdateLook(double xPos, double yPos);
    // Call when the cursor is released so the next look update doesn't jump.
    void ResetLook() { firstMouse = true; }
    void SetPosition(const glm::vec3& eye) { position = eye; }

    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix(float aspectRatio) const;

    glm::vec3 GetPosition() const { return position; }
    glm::vec3 GetDirection() const { return front; }
    glm::vec2 GetDirectionAngles() const { return { pitch, yaw }; }
    float GetYaw() const { return yaw; }
    void SetYaw(float degrees);
    void SetLook(float yawDegrees, float pitchDegrees);
    // Vertical field of view (degrees) and mouse sensitivity (degrees per pixel).
    void SetFov(float degrees) { fov = degrees; }
    void SetSensitivity(float degreesPerPixel) { sensitivity = degreesPerPixel; }

private:
    void UpdateFront();

    glm::vec3 position{7.0f, 7.0f, 7.0f};
    glm::vec3 front{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};

    bool firstMouse = true;
    double lastX = 0.0, lastY = 0.0;
    float pitch = 0.0f, yaw = -90.0f;
    float fov = 45.0f, sensitivity = 0.1f;
};
