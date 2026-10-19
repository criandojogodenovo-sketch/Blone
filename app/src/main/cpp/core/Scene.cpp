#include "core/Scene.h"

#include <algorithm>
#include <cmath>

namespace blone {

SceneLines buildGrid(float y, int extent, int step) {
    SceneLines out;
    if (step <= 0 || extent <= 0) return out;
    out.vertices.reserve(static_cast<size_t>((2 * extent / step + 1) * 2) * 2);

    // Subtle blue-gray grid, slightly transparent so the dark background reads through.
    const float r = 0.30f, g = 0.34f, b = 0.42f, a = 0.85f;

    for (int i = -extent; i <= extent; i += step) {
        const float p = static_cast<float>(i);
        // Line parallel to X at z = p.
        out.vertices.push_back({static_cast<float>(-extent), y, p, r, g, b, a});
        out.vertices.push_back({static_cast<float>(extent), y, p, r, g, b, a});
        // Line parallel to Z at x = p.
        out.vertices.push_back({p, y, static_cast<float>(-extent), r, g, b, a});
        out.vertices.push_back({p, y, static_cast<float>(extent), r, g, b, a});
    }
    return out;
}

SceneLines buildAxes(float y, float positiveLength, float negativeLength) {
    SceneLines out;
    const float axes[3][3] = {
            {0.95f, 0.30f, 0.30f},   // X — red
            {0.35f, 0.85f, 0.40f},   // Y — green
            {0.35f, 0.55f, 0.95f},   // Z — blue
    };

    for (int axis = 0; axis < 3; ++axis) {
        const float r = axes[axis][0];
        const float g = axes[axis][1];
        const float b = axes[axis][2];

        Vec3 dir{0, 0, 0};
        dir.x = (axis == 0) ? 1.0f : 0.0f;
        dir.y = (axis == 1) ? 1.0f : 0.0f;
        dir.z = (axis == 2) ? 1.0f : 0.0f;

        const Vec3 origin{0.0f, y, 0.0f};
        const Vec3 posEnd = origin + dir * positiveLength;
        const Vec3 negEnd = origin - dir * negativeLength;

        // Positive half: full color. Negative half: dimmed for orientation context.
        out.vertices.push_back({origin.x, origin.y, origin.z, r, g, b, 1.0f});
        out.vertices.push_back({posEnd.x, posEnd.y, posEnd.z, r, g, b, 1.0f});
        out.vertices.push_back({origin.x, origin.y, origin.z, r * 0.4f, g * 0.4f, b * 0.4f, 0.45f});
        out.vertices.push_back({negEnd.x, negEnd.y, negEnd.z, r * 0.4f, g * 0.4f, b * 0.4f, 0.45f});
    }
    return out;
}

bool rayIntersectsAabb(const Vec3& rayOrigin,
                       const Vec3& rayDir,
                       const Aabb& box,
                       float maxDistance) {
    float tMin = 0.0f;
    float tMax = maxDistance;

    const float origin[3] = {rayOrigin.x, rayOrigin.y, rayOrigin.z};
    const float dir[3] = {rayDir.x, rayDir.y, rayDir.z};
    const float bmin[3] = {box.minX, box.minY, box.minZ};
    const float bmax[3] = {box.maxX, box.maxY, box.maxZ};

    for (int i = 0; i < 3; ++i) {
        if (std::fabs(dir[i]) < 1e-9f) {
            // Ray parallel to this axis: must originate inside the slab.
            if (origin[i] < bmin[i] || origin[i] > bmax[i]) return false;
        } else {
            const float inv = 1.0f / dir[i];
            float t1 = (bmin[i] - origin[i]) * inv;
            float t2 = (bmax[i] - origin[i]) * inv;
            if (t1 > t2) std::swap(t1, t2);
            tMin = std::max(tMin, t1);
            tMax = std::min(tMax, t2);
            if (tMin > tMax) return false;
        }
    }
    return true;
}

} // namespace blone
