#pragma once
// Brush engine (Fase 3): sculpting and painting on the editable half-edge
// mesh. This is NOT a second geometry system — it operates directly on the
// existing HalfEdgeMesh vertices and reuses the existing scene/history
// infrastructure.
//
// Contracts (pinned by host tests, tests/test_brush.cpp):
//  - collectAffected() selects every ALIVE vertex whose distance to the hit
//    point (LOCAL space) is <= radius. No vertex outside the radius is ever
//    returned; the mesh is never mutated by this function.
//  - sculptApply() displaces ONLY the given vertices along the NORMAL OF
//    THE HIT FACE (the surface actually under the finger, unit-normalized
//    inside — see the note below for why the per-vertex normal is NOT
//    used). The displacement is applied per vertex with a smooth falloff
//    that is 1 at the hit point and 0 at the radius edge. After the call
//    every affected position is finite; if any non-finite value would be
//    produced the whole application is REVERTED and false is returned
//    (mesh intact).
//  - paintApply() mixes the brush color into the vertex colors of the given
//    vertices with factor = opacity * falloff. Outside vertices keep their
//    exact color. Non-finite results are reverted the same way.
//  - Both functions reject invalid settings/hit points up front (return
//    false / empty) — NaN/Inf radius, strength, color or hit point never
//    reach the mesh.

#include "../Math3D.h"
#include "../mesh/HalfEdgeMesh.h"

#include <vector>

namespace blone::brush {

// Surface displacement mode ("modo de deslocamento da superfície").
enum class SculptMode : int { Raise = 0, Dig = 1 };

// Real parameters of a stroke (NOT merely visual values): radius gates the
// affected vertices, strength scales the displacement, opacity scales the
// color blend. Values are validated by settingsValid() and clamped by
// sanitized() before every use.
struct BrushSettings {
    float radius = 0.5f;          // world units, > 0
    float strength = 0.5f;        // [0.01, 1]
    float opacity = 0.8f;         // [0.01, 1] (paint)
    SculptMode mode = SculptMode::Raise;
    Vec3 color = kDefaultVertexColor; // albedo RGB in [0,1] (paint)
};

bool settingsValid(const BrushSettings& s);
// Returns a finite, in-range copy of `s`; invalid (non-finite/out-of-range)
// components fall back to safe defaults instead of propagating garbage.
BrushSettings sanitized(const BrushSettings& s);

// Vertex normal (area-weighted) is NOT used for displacement: sculpting
// displaces along the NORMAL OF THE HIT FACE (the surface actually under the
// finger), which keeps the deformation predictable on coarse meshes (a cube
// corner vertex would otherwise average 3 faces and slide diagonally).

// Smooth radial falloff in [0,1]: 1 at dist 0, 0 at dist >= radius.
float falloff(float dist, float radius);

// Fills `out` (cleared first) with the alive vertices inside the brush
// sphere. Ordered by mesh pool order; the caller deduplicates across stroke
// steps. Never mutates the mesh.
void collectAffected(const HalfEdgeMesh& m, const Vec3& hitLocal, float radius,
                     std::vector<ElemId>& out);

// Displaces `verts` along the hit-face normal (unit-normalized inside).
// `hitLocal` anchors the falloff; `hitNormal` is the surface normal of the
// face under the brush (NOT normalized on input). Returns false (and
// restores the previous positions) when the settings, hit point or normal
// are invalid, or when a non-finite position would be written.
bool sculptApply(HalfEdgeMesh& m, const std::vector<ElemId>& verts,
                 const Vec3& hitLocal, const Vec3& hitNormal,
                 const BrushSettings& s);

// Blends the brush color into `verts` colors. Same validation/revert
// contract as sculptApply().
bool paintApply(HalfEdgeMesh& m, const std::vector<ElemId>& verts,
                const Vec3& hitLocal, const BrushSettings& s);

} // namespace blone::brush
