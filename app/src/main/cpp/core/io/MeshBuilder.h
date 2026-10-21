#pragma once
// core/io — Fase 4: mesh I/O (OBJ import/export, glTF/GLB import).
//
// MeshBuilder: the ONLY bridge between file formats and the live scene.
// It consumes a RawMeshData (a flat, format-agnostic description) and
// produces a validated HalfEdgeMesh through the EXISTING construction
// pipeline (addVertex -> addFace -> relinkBoundaryLoops -> validate).
// It is NOT a second geometry system: on any rejection the function returns
// std::nullopt and NOTHING exists outside the local (discarded) temporary —
// all-or-nothing by construction (Regra 2).
//
// RawMeshData contract:
//  - positions is the vertex pool; faceIndices reference it by 0-based
//    index (one inner vector per face).
//  - normals / colors / uvs are either EMPTY or exactly parallel to
//    positions.
//  - colors: an EMPTY array means "all white" (RGB {1,1,1}) — every vertex
//    of the built mesh therefore always carries a defined paint color.
//  - uvs use the local Vec2 (core/Math3D.h deliberately has no Vec2).
//
// Rejections (std::nullopt, zero side effects):
//  - empty positions or empty faces;
//  - MeshLimits breached (maxVertices, maxFaces, maxFaceSides);
//  - parallel arrays inconsistent (present but not parallel to positions);
//  - non-finite positions (NaN/Inf); non-finite colors are CLAMPED to
//    [0,1] only if finite, otherwise rejected;
//  - any face with fewer than 3 or more than maxFaceSides corners;
//  - any index outside [0, positions.size());
//  - a vertex repeated inside one face loop;
//  - addFace() failure (e.g. non-manifold edge) — addFace already
//    guarantees zero mutation on failure;
//  - post-build validate() failure (defensive; cannot normally happen
//    after successful addFace calls).
//
// Normals/uvs are validated for parallelism only (they are NOT stored in
// the HalfEdgeMesh — the engine computes smooth normals itself and UV
// storage is a documented Fase 5 extension point).

#include "../mesh/HalfEdgeMesh.h"

#include <optional>
#include <string>
#include <vector>

namespace blone::io {

// Texture coordinate (Math3D.h has no Vec2 by design; this type is local
// to the I/O layer and never enters the engine).
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

// Flat, format-agnostic geometry description produced by the loaders and
// consumed by MeshBuilder (see header comment for the exact contract).
struct RawMeshData {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec3> colors; // albedo RGB in [0,1]; empty = all white
    std::vector<Vec2> uvs;
    std::vector<std::vector<int>> faceIndices; // one loop per face, 0-based
};

class MeshBuilder {
public:
    // Builds a validated HalfEdgeMesh from `data`. Returns std::nullopt and
    // has zero side effects on every rejection listed above.
    static std::optional<HalfEdgeMesh> build(const RawMeshData& data);
};

} // namespace blone::io
