#pragma once
// core/render — READ-ONLY render mesh (missão F4-LIMITS, P3).
//
// Imports above the profile's EDIT budget but within the VIEW budget
// (core/io/GeometryLimits.h) become view-only objects: NO HalfEdgeMesh is
// built for them (the half-edge pool + pairMap is exactly the memory that
// used to crash). The geometry is an indexed triangle soup the renderer
// uploads once — ~30 B per triangle versus ~270 B per editable triangle —
// and flat shading comes from screen-space derivatives in the fragment
// shader (no duplicated fill buffers).
//
// Contract:
//  - positions/colors are copied VERBATIM from the loader output (the
//    promotion round-trip is therefore bit-exact by construction);
//  - polygon faces are fan-triangulated for VIEWING (n-2 triangles — the
//    same count io::rawTriangleCount reports). Concave polygons can fan
//    imperfectly: documented view-only limitation; promotion rebuilds the
//    exact polygonal topology through MeshBuilder;
//  - degenerate triangles (repeated corner indices) are skipped;
//  - immutable after build; history entries share it via std::shared_ptr
//    so undo/redo of read imports never duplicates geometry;
//  - centroid + bounds are computed ONCE here (O(V)) — the gizmo pivot
//    rule needs them every frame and must not walk 500k vertices per frame.

#include "../Math3D.h"
#include "../io/MeshBuilder.h" // RawMeshData (loader output contract)

#include <cstdint>
#include <vector>

namespace blone::render {

struct RenderMesh {
    std::vector<Vec3> positions;   // unique loader vertices, LOCAL space
    std::vector<Vec3> colors;      // parallel albedo; empty = all white
    std::vector<uint32_t> indices; // 3 per triangle, into positions
    Aabb bounds{};
    Vec3 centroid{};
    size_t triangleCount = 0;

    bool empty() const { return positions.empty() || indices.empty(); }
    // Exact heap bytes held by this mesh (documentation + history budget).
    uint64_t memoryBytes() const;
};

// Builds the read-only mesh from loader output. Allocation failures surface
// as std::bad_alloc and are handled by the I/O worker (P4 anti-crash).
RenderMesh buildRenderMesh(const blone::io::RawMeshData& raw);

// Rebuilds the loader-level description (P5: export path + promotion input).
// Each triangle becomes one 3-corner face; positions/colors are verbatim.
blone::io::RawMeshData renderMeshToRaw(const RenderMesh& m);

} // namespace blone::render
