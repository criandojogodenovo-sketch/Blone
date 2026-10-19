#pragma once
// Mesh-level modeling operations. Contract for every operation:
//  - Validation happens BEFORE any mutation; validation failures leave the
//    mesh untouched (no partial state).
//  - After mutation the mesh is validate()d; on failure the operation rolls
//    back from its own snapshot and returns ErrInternal.
//  - Success returns OK and the mesh satisfies all half-edge invariants.

#include "../mesh/HalfEdgeMesh.h"

#include <set>
#include <vector>

namespace blone::ops {

// Status codes shared with the scene layer and JNI.
enum class Status : int {
    OK = 0,
    ErrNoSelection = 1,    // nothing selected
    ErrNoActiveObject = 2, // no active object
    ErrUnsupported = 3,    // topology does not support the operation
    ErrNonManifold = 4,    // delete would produce unsupported topology
    ErrBudget = 5,         // history budget exhausted (rejected before mutating)
    ErrLimit = 6,          // geometry limits breached
    ErrInternal = 7,       // post-validation failed (mesh rolled back)
    ErrInvalidArg = 8,
};

// ---------------------------------------------------------------- extrude --
// Extrudes the given face set as a region: the region faces are removed, side
// walls are built along every region-boundary edge, and a cap (copy of the
// region) is created on duplicated vertices. On success `capFacesOut` holds
// the new cap faces (selection moves there). Closed regions without boundary
// (e.g. the whole cube) are rejected: ErrUnsupported.
Status extrudeFaces(HalfEdgeMesh& m, const std::vector<ElemId>& faces,
                    std::set<ElemId>& capFacesOut);

// -------------------------------------------------------------- subdivide --
// Linear 1 -> 4 subdivision (the documented Phase 2 rule):
//   quad -> 4 quads via the 4 edge midpoints + the face centroid
//   tri  -> 4 tris  via the 3 edge midpoints
// Midpoints are shared between adjacent faces (split edges). Neighbour faces
// outside the set keep their shape: they gain loop vertices where shared
// edges were split (valid polygons, documented behaviour). Only quads and
// tris are supported; anything else is rejected up front.
Status subdivideFaces(HalfEdgeMesh& m, const std::vector<ElemId>& faces);

// ------------------------------------------------------------------ delete --
enum class DelKind { Vertices = 0, Edges = 1, Faces = 2 };
// Deletes the requested elements respecting topological dependencies:
//  - vertex: its star faces are removed; the link of the vertex must be a
//    single cycle or a single path, otherwise ErrNonManifold (rejected).
//  - edge (2 faces): the two faces merge into one polygon; they must share
//    exactly one edge and the merge must stay within face size limits.
//  - edge (boundary): the single adjacent face is removed together with it.
//  - face: removed; dependent edges/vertices cleaned up.
// The whole selection is validated first; one invalid element rejects the
// entire operation with the mesh untouched.
Status deleteElements(HalfEdgeMesh& m, DelKind kind, const std::vector<ElemId>& sel);

// ---------------------------------------------------------------- loop cut --
struct LoopCutReport {
    std::vector<ElemId> ringFaces;    // faces divided by the cut
    std::vector<ElemId> crossedEdges; // edges split by the cut (old ids)
    std::vector<ElemId> newVertices;  // midpoints created on crossed edges
    std::vector<ElemId> newLoopEdges; // new edges along the cut line
};
// Cuts a ring of quads through `edge` (which is crossed, i.e. split).
// Definition (documented and pinned by tests on the cube):
//   - starting at the selected edge, the ring walk enters each adjacent quad
//     and exits through the OPPOSITE edge, crossing it as well;
//   - the walk continues until it returns to the start (closed ring) or hits
//     a boundary edge (open ring, terminates there);
//   - every ring face is divided into two quads connected by a new edge
//     between the two crossed-edge midpoints;
//   - faces adjacent to crossed edges but outside the ring keep their shape
//     (they gain loop vertices, becoming pentagons/hexagons — valid);
//   - non-quad faces on the ring, or a walk that neither closes nor reaches
//     a boundary, are REJECTED with the mesh untouched.
Status loopCut(HalfEdgeMesh& m, ElemId edge, LoopCutReport& rep);

} // namespace blone::ops
