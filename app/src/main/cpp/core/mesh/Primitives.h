#pragma once
// Primitive generators producing valid editable HalfEdgeMesh objects.
// All generators build into a temporary mesh and only then swap, so a failed
// build never leaves partial state. Parameters are validated against safe
// limits to protect low-RAM devices.

#include "HalfEdgeMesh.h"

namespace blone {

enum class PrimType { Cube = 0, Sphere = 1, Cylinder = 2, Plane = 3 };

struct PrimitiveParams {
    float cx = 0.0f, cy = 1.0f, cz = 0.0f; // center (cube/sphere/cylinder) or plane center
    float size = 1.0f;                     // half extent (cube/plane) or radius (sphere/cylinder)
    int segsU = 16;                        // cylinder/sphere segments around the axis
    int segsV = 8;                         // sphere latitude bands (>=2)
};

// Safe parameter ranges (documented in ARCHITECTURE.md).
inline constexpr int kMinSegmentsU = 3;
inline constexpr int kMaxSegmentsU = 96;
inline constexpr int kMinSegmentsV = 2;
inline constexpr int kMaxSegmentsV = 48;
inline constexpr float kMinSize = 0.01f;
inline constexpr float kMaxSize = 64.0f;

// Returns false (leaving `out` untouched) when parameters are out of range or
// the element limits would be breached.
bool buildPrimitive(PrimType type, const PrimitiveParams& p, HalfEdgeMesh& out);

} // namespace blone
