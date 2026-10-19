#pragma once
// Pure screen-drag -> world mapping for the explicit transform tools
// (Mover/Rodar/Escalar). Camera gestures never use this file: two fingers
// remain reserved for the camera (see ARCHITECTURE.md).
//
// Documented mappings (pinned by unit tests):
//   Mover   : the selection follows the finger (drag right moves it right on
//             screen, drag up moves it up), scaled by world-units-per-pixel
//             at the orbit distance.
//   Rodar   : horizontal drag rotates around world Y (-0.4 deg/px),
//             vertical drag rotates around the camera right axis
//             (+0.4 deg/px); pivot = selection centroid.
//   Escalar : vertical drag scales uniformly around the selection centroid,
//             factor = clamp(1 - 0.005 * dyPx, 0.05, 20) (drag up = grow).

#include "../Math3D.h"
#include "../Camera.h"
#include "../mesh/HalfEdgeMesh.h"

#include <algorithm>
#include <vector>

namespace blone::ops {

struct DragParams {
    float azimuthDeg = 35.0f;
    float elevationDeg = 25.0f;
    float distance = 8.0f;
    float fovYDeg = 45.0f;
    float viewportHeightPx = 1.0f;
};

inline OrbitCamera cameraFromParams(const DragParams& p) {
    OrbitCamera c;
    c.azimuthDeg = p.azimuthDeg;
    c.elevationDeg = p.elevationDeg;
    c.distance = p.distance;
    c.fovYDeg = p.fovYDeg;
    return c;
}

inline float worldPerPx(const DragParams& p) {
    if (p.viewportHeightPx <= 0.0f) return 0.0f;
    const float rad = p.fovYDeg * static_cast<float>(M_PI) / 180.0f;
    return 2.0f * p.distance * std::tan(rad * 0.5f) / p.viewportHeightPx;
}

inline Vec3 moveWorldDelta(const DragParams& p, float dxPx, float dyPx) {
    const OrbitCamera c = cameraFromParams(p);
    const float w = worldPerPx(p);
    return c.right() * (dxPx * w) + c.up() * (-dyPx * w);
}

struct DragRotation {
    Vec3 axisY{};     // world Y
    float degY = 0;   // from horizontal drag
    Vec3 axisRight{}; // camera right
    float degRight = 0; // from vertical drag
};

inline DragRotation rotationFromDrag(const DragParams& p, float dxPx, float dyPx) {
    const OrbitCamera c = cameraFromParams(p);
    DragRotation r;
    r.axisY = Vec3{0.0f, 1.0f, 0.0f};
    r.degY = -dxPx * 0.4f;
    r.axisRight = c.right();
    r.degRight = dyPx * 0.4f;
    return r;
}

inline float scaleFromDrag(float dyPx) {
    return std::clamp(1.0f - dyPx * 0.005f, 0.05f, 20.0f);
}

// Vertex-set application helpers (dedup is the caller's duty).
inline void applyMove(HalfEdgeMesh& m, const std::vector<ElemId>& verts, const Vec3& d) {
    for (const ElemId v : verts) {
        if (m.vertexAlive(v)) m.vrt(v).pos = m.vrt(v).pos + d;
    }
}

inline void applyRotation(HalfEdgeMesh& m, const std::vector<ElemId>& verts,
                          const Vec3& pivot, const Vec3& axis, float deg) {
    const Mat4 r = Mat4::rotationAxisDeg(axis, deg);
    for (const ElemId v : verts) {
        if (!m.vertexAlive(v)) continue;
        const Vec3 rel = m.vrt(v).pos - pivot;
        m.vrt(v).pos = pivot + r.transformPoint(rel);
    }
}

inline void applyScale(HalfEdgeMesh& m, const std::vector<ElemId>& verts,
                       const Vec3& pivot, float factor) {
    for (const ElemId v : verts) {
        if (!m.vertexAlive(v)) continue;
        const Vec3 rel = m.vrt(v).pos - pivot;
        m.vrt(v).pos = pivot + rel * factor;
    }
}

} // namespace blone::ops
