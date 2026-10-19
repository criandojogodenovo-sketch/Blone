#pragma once
// Scene helpers that are not GL-specific: grid/axes line geometry and
// ray picking against object bounds.

#include "core/Math3D.h"
#include "core/Mesh.h"

#include <vector>

namespace blone {

// Interleaved line vertex: pos(3) + rgba(4).
struct LineVertex {
    float x, y, z;
    float r, g, b, a;
};

struct SceneLines {
    std::vector<LineVertex> vertices; // 2 vertices per line segment
};

// Builds the ground grid on the XZ plane at height y.
// extent: half-size in world units; step: spacing between lines.
SceneLines buildGrid(float y, int extent, int step);

// Builds the world axes through (0, y, 0): X red, Y green, Z blue.
// Positive half is opaque; the negative half is drawn dimmer.
SceneLines buildAxes(float y, float positiveLength, float negativeLength);

// Ray/AABB slab intersection. Returns true when [tMin, tMax] with tMin >= 0
// overlaps the box. rayDir must be normalized.
bool rayIntersectsAabb(const Vec3& rayOrigin,
                       const Vec3& rayDir,
                       const Aabb& box,
                       float maxDistance);

} // namespace blone
