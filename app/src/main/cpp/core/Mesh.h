#pragma once
// Static mesh builders. All geometry is generated once at startup.

#include <cstdint>
#include <vector>

namespace blone {

// Axis-aligned bounds of the cube, used for picking and edge highlighting.
struct Aabb {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
};

struct CubeMesh {
    // Interleaved: pos(3) + normal(3), 24 vertices (4 per face).
    std::vector<float> vertices;
    // 36 indices (6 per face), uint16.
    std::vector<uint16_t> indices;
    Aabb bounds;
};

// Builds a cube of the given half-extent centered at (cx, cy, cz) with per-face
// normals suitable for flat lighting.
CubeMesh buildCube(float cx, float cy, float cz, float halfExtent);

} // namespace blone
