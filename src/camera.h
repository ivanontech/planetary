#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

// Camera ported from original Planetary's arcball + zoom system
// Original: G_INIT_CAM_DIST = 250, FOV 55-80, easing 10% per frame
class Camera {
public:
    glm::vec3 position{20.0f, 100.0f, 60.0f};
    glm::vec3 target{0.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};

    // Target for smooth interpolation (original: 10% per frame easing)
    glm::vec3 targetPos{20.0f, 100.0f, 60.0f};
    glm::vec3 targetLookAt{0.0f, 0.0f, 0.0f};

    float fov = 60.0f;     // G_DEFAULT_FOV
    float nearPlane = 0.01f;
    float farPlane = 2000.0f;
    float aspect = 16.0f / 9.0f;

    // Orbit control
    float orbitYaw = 0.3f;
    float orbitPitch = 1.2f;
    float orbitDist = 150.0f;
    float targetOrbitDist = 150.0f;

    float autoRotateSpeed = 0.02f; // Slow auto-rotation at galaxy level
    bool autoRotate = true;

    void update(float dt) {
        if (autoRotate) {
            orbitYaw += autoRotateSpeed * dt;
        }

        // Smooth zoom
        orbitDist += (targetOrbitDist - orbitDist) * 4.0f * dt;

        // Compute position from orbit angles
        float x = orbitDist * cosf(orbitPitch) * sinf(orbitYaw);
        float y = orbitDist * sinf(orbitPitch);
        float z = orbitDist * cosf(orbitPitch) * cosf(orbitYaw);

        glm::vec3 desiredPos = targetLookAt + glm::vec3(x, y, z);

        // Smooth interpolation (original: 10% per frame)
        position += (desiredPos - position) * glm::min(4.0f * dt, 1.0f);
        target += (targetLookAt - target) * glm::min(4.0f * dt, 1.0f);
    }

    glm::mat4 viewMatrix() const {
        return glm::lookAt(position, target, up);
    }

    glm::mat4 projMatrix() const {
        return glm::perspective(glm::radians(fov), aspect, nearPlane, farPlane);
    }

    void onMouseDrag(float dx, float dy) {
        orbitYaw -= dx * 0.005f;
        orbitPitch += dy * 0.005f;
        orbitPitch = glm::clamp(orbitPitch, 0.05f, 1.5f);
    }

    void onMouseScroll(float delta) {
        targetOrbitDist *= (1.0f - delta * 0.1f);
        targetOrbitDist = glm::clamp(targetOrbitDist, 1.0f, 500.0f);
    }

    // Navigate to a specific point (like selecting a star)
    void flyTo(glm::vec3 pos, float dist) {
        targetLookAt = pos;
        targetOrbitDist = dist;
    }
};
