#pragma once
// Static mesh builders. All geometry is generated once at startup.
// Kept for the Phase 1 demo cube; the editable topology lives in
// core/mesh/HalfEdgeMesh.h since Phase 2.

#include "Math3D.h"

#include <cstdint>
#include <vector>

namespace blone {

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
