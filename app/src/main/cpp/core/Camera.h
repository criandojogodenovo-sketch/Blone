#pragma once
// Orbit camera used by the viewport. State is pushed from Kotlin every frame;
// this class only derives derived quantities (eye, basis, view matrix).

#include "core/Math3D.h"

#include <cmath>

namespace blone {

struct OrbitCamera {
    float azimuthDeg = 35.0f;
    float elevationDeg = 25.0f;
    float distance = 8.0f;
    Vec3 target{0.0f, 1.0f, 0.0f};
    float fovYDeg = 45.0f;

    // Unit direction from the target towards the camera.
    Vec3 orbitDirection() const {
        const float az = azimuthDeg * static_cast<float>(M_PI) / 180.0f;
        const float el = elevationDeg * static_cast<float>(M_PI) / 180.0f;
        const float ce = std::cos(el);
        return {ce * std::sin(az), std::sin(el), ce * std::cos(az)};
    }

    Vec3 eye() const { return target + orbitDirection() * distance; }
    Vec3 forward() const { return normalize(target - eye()); }

    Vec3 right() const {
        const Vec3 worldUp{0.0f, 1.0f, 0.0f};
        return normalize(cross(forward(), worldUp));
    }

    Vec3 up() const { return normalize(cross(right(), forward())); }

    Mat4 viewMatrix() const {
        const Vec3 worldUp{0.0f, 1.0f, 0.0f};
        return Mat4::lookAt(eye(), target, worldUp);
    }
};

} // namespace blone
