#pragma once
// Half-edge (DCEL) editable mesh — the core topology structure of Blone.
//
// Representation (documented in ARCHITECTURE.md):
//  - Elements carry persistent ids (monotonic counters, never reused in a
//    session). Id 0 is the null id.
//  - A half-edge with face == 0 is a BOUNDARY half-edge; boundary half-edges
//    form closed loops linked by next/prev, so border walking is uniform.
//  - Every alive edge owns exactly two half-edges (twin pair). An alive edge
//    always has at least one adjacent face; edges with zero faces are removed.
//  - Faces are dynamic polygons (3..Limits::maxFaceSides sides). Triangles
//    appear as primitives/export views; the editable structure stays polygonal.
//  - Vertex.outgoing is ONE alive half-edge with origin == vertex (no ring
//    order maintained on purpose: incident-face queries scan faces, which is
//    fine for user-triggered operations and picking at Blone's scale).
//
// Per-corner texture coordinates (Fase 5.1): a Vec2 per half-edge, indexed
// by the half-edge ElemId (the id space is monotonic and never reused, so
// the array is "parallel to the half-edges" without following the slot
// recycling of the pools). STORAGE IS LAZY: the array stays EMPTY until a
// face is added WITH UVs (addFace overload below) — meshes without UV data
// (every Fase 2-4 construction path) pay zero bytes and behave exactly as
// before. A half-edge whose id is outside the array carries the default
// (0,0); the FACE-SIDE half-edge of a corner holds that corner's UV, so a
// geometric vertex shared by two faces with different UVs (a UV seam) keeps
// one value per face — impossible with per-vertex storage.
//
// Invariants enforced by validate() and preserved by every operation:
//  I1  twin symmetry: he.twin.twin == he
//  I2  next/prev consistency inside every loop (face loops and boundary loops)
//  I3  every half-edge of a face loop references that face; face.he belongs
//      to its own loop; boundary half-edges have face == 0
//  I4  every alive edge has 1 or 2 adjacent faces, and its two half-edges
//      never belong to the same face (no "fin" edges)
//  I5  no dangling references: origin/twin/next/prev/face/edge/outgoing point
//      to alive elements (or 0 for face on boundary half-edges)
//  I6  no orphan elements: every alive vertex has outgoing != 0, every alive
//      face has he != 0, every alive edge has he != 0
//  I7  face loops have 3..maxFaceSides distinct vertices

#include "../Math3D.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace blone {

using ElemId = uint32_t;
inline constexpr ElemId kNull = 0;

// Base material color carried by every NEW vertex (and by vertices loaded
// from v1 project files). It matches the historical fill-shader base color,
// so unpainted meshes render exactly as before the vertex-color extension.
inline constexpr Vec3 kDefaultVertexColor{0.58f, 0.66f, 0.78f};

// HARD geometry ceiling (missão F4-LIMITS, P1): absolute safety net of the
// engine pools, chosen so a mesh at the ceiling still fits a high-RAM device
// and ElemId (uint32) keeps headroom. This is NOT the user-facing limit —
// the RUNTIME limits per device profile (core/io/GeometryLimits.h) decide
// edit vs read-only vs reject long before this ceiling is approached.
// Memory at the ceiling is far beyond any profile budget: a build that gets
// here is always rejected by the runtime gate first; these constants exist
// so even a bug upstream can never run the pools unbounded.
struct MeshLimits {
    static constexpr size_t maxVertices  = 4000000;
    static constexpr size_t maxEdges     = 12000000;
    static constexpr size_t maxHalfEdges = 24000000;
    static constexpr size_t maxFaces     = 8000000;
    static constexpr size_t maxFaceSides = 256;
};

class HalfEdgeMesh {
public:
    struct Vertex {
        ElemId id = kNull;
        Vec3 pos{};
        // Paint color (Fase 3). Positions are LOCAL; colors are albedo RGB
        // in [0,1]. Default keeps the historical shader base color so old
        // scenes and unpainted meshes look unchanged.
        Vec3 color = kDefaultVertexColor;
        ElemId outgoing = kNull;
        bool alive = false;
    };
    struct HalfEdge {
        ElemId id = kNull;
        ElemId origin = kNull; // vertex this half-edge starts from
        ElemId twin = kNull;
        ElemId next = kNull;
        ElemId prev = kNull;
        ElemId face = kNull;   // 0 => boundary half-edge
        ElemId edge = kNull;
        bool alive = false;
    };
    struct Edge {
        ElemId id = kNull;
        ElemId he = kNull; // one of the two half-edges
        bool alive = false;
    };
    struct Face {
        ElemId id = kNull;
        ElemId he = kNull; // one half-edge of the loop
        bool alive = false;
    };

    // ---------- construction ----------
    ElemId addVertex(const Vec3& p);                 // 0 when limits breached
    // Adds a face through the given vertex loop (CCW seen from outside).
    // Reuses alive boundary half-edges when the opposite direction is free,
    // creates the missing ones. Returns 0 on failure WITHOUT mutating
    // (non-manifold request, duplicate vertices, size limits).
    ElemId addFace(const std::vector<ElemId>& loop);
    // Fase 5.1: same construction, plus one UV per corner. cornerUvs must
    // be EXACTLY parallel to loop (else 0, no mutation — same fail-clean
    // contract). cornerUvs[i] is the UV of the corner AT loop[i] and is
    // stored on the face-side half-edge leaving loop[i]; created boundary
    // twins get the (0,0) default until a face claims them. This is the
    // ONLY path that grows the uv array (splitEdge interpolates within it).
    ElemId addFace(const std::vector<ElemId>& loop,
                   const std::vector<Vec2>& cornerUvs);
    // Splits an edge at its midpoint: creates the mid vertex and two edges.
    // Adjacent face loops (and boundary loop) are rewired in place.
    bool splitEdge(ElemId edgeId, ElemId& midVert, ElemId& edgeA, ElemId& edgeB);

    // ---------- queries ----------
    bool vertexAlive(ElemId id) const;
    bool halfEdgeAlive(ElemId id) const;
    bool edgeAlive(ElemId id) const;
    bool faceAlive(ElemId id) const;

    const Vertex& vrt(ElemId id) const { return verts_[vSlot_[id]]; }
    Vertex& vrt(ElemId id) { return verts_[vSlot_[id]]; }
    const HalfEdge& heg(ElemId id) const { return hes_[hSlot_[id]]; }
    HalfEdge& heg(ElemId id) { return hes_[hSlot_[id]]; }
    const Edge& edg(ElemId id) const { return edges_[eSlot_[id]]; }
    Edge& edg(ElemId id) { return edges_[eSlot_[id]]; }
    const Face& fac(ElemId id) const { return faces_[fSlot_[id]]; }
    Face& fac(ElemId id) { return faces_[fSlot_[id]]; }

    // Destination vertex of a half-edge (origin of next).
    ElemId heDest(ElemId heId) const { return heg(heg(heId).next).origin; }

    // ---------- per-corner UVs (Fase 5.1) ----------
    // UV of a half-edge corner. Half-edges without stored data (mesh never
    // given UVs, or corner created by a no-UV path) return {0,0}.
    Vec2 uv(ElemId heId) const {
        return heId < uvs_.size() ? uvs_[heId] : Vec2{0.0f, 0.0f};
    }
    // True once any UV was stored — decides whether the OBJ exporter emits
    // `vt` lines / `f v/vt/vn` corners or keeps the historical `f v//vn`.
    bool hasUvs() const { return !uvs_.empty(); }
    // Read-only access for serialization (Project v4) and debug tools;
    // indexed by half-edge id, size() <= nextHid (see header comment).
    const std::vector<Vec2>& uvs() const { return uvs_; }

    // Returns an alive directed half-edge a -> b, or 0. O(1) via pairMap_.
    ElemId findDirectedHalfEdge(ElemId a, ElemId b) const;

    std::vector<ElemId> faceVertices(ElemId f) const;
    std::vector<ElemId> faceHalfEdges(ElemId f) const;
    std::vector<ElemId> faceEdges(ElemId f) const;
    std::vector<ElemId> vertexIncidentFaces(ElemId v) const; // O(faces)
    std::vector<ElemId> vertexNeighbors(ElemId v) const;     // O(faces)
    size_t faceSideCount(ElemId f) const;

    int edgeFaceCount(ElemId e) const;                 // 1 or 2
    bool isBoundaryEdge(ElemId e) const;               // exactly 1 face
    ElemId edgeOtherFace(ElemId e, ElemId f) const;    // 0 when boundary
    ElemId oppositeEdgeInQuad(ElemId face, ElemId edge) const; // kNull if not a quad

    Vec3 faceNormal(ElemId f) const;   // Newell, normalized
    Vec3 faceCentroid(ElemId f) const; // average of loop vertices
    Aabb computeBounds() const;

    size_t aliveVertexCount() const { return aliveV_; }
    size_t aliveEdgeCount() const { return aliveE_; }
    size_t aliveHalfEdgeCount() const { return aliveH_; }
    size_t aliveFaceCount() const { return aliveF_; }
    bool empty() const { return aliveV_ == 0; }

    // Ids of every alive face (unordered, ascending slot order).
    std::vector<ElemId> aliveFaceIds() const;

    // Read-only pool access (serialization, debug tools).
    const std::vector<Vertex>& vertices() const { return verts_; }
    const std::vector<HalfEdge>& halfEdges() const { return hes_; }
    const std::vector<Edge>& edges() const { return edges_; }
    const std::vector<Face>& faces() const { return faces_; }

    // True when the vertex lies on the mesh boundary (some incident half-edge
    // has face == 0).
    bool isBoundaryVertex(ElemId v) const;

    // ---------- deletion (low level; operations validate beforehand) ----------
    // Unlinks the face loop (its half-edges become boundary), removes edges
    // left without faces and vertices left isolated.
    void removeFaceAndCleanup(ElemId f);
    // Removes ALL incident faces of the vertex, then the vertex itself.
    void removeVertexWithFaces(ElemId v);
    void removeIsolatedElements();
    // Kills an alive edge, its two half-edges and one face (used by the
    // edge-merge deletion). Maintains free lists/alive counters and repairs
    // vertex.outgoing of the edge endpoints. The caller must have relinked
    // the surviving face loop beforehand.
    void destroyEdgeWithFace(ElemId edgeId, ElemId faceId);

    // Rebuilds next/prev of ALL boundary half-edges from scratch (O(H)).
    // Boundary half-edges created by addFace are unlinked until this runs;
    // every mutating operation MUST call it right before validate().
    void relinkBoundaryLoops();

    // ---------- validation ----------
    bool validate() const;

    // ---------- snapshot / memory accounting ----------
    struct Raw {
        std::vector<Vertex> verts;
        std::vector<HalfEdge> hes;
        std::vector<Edge> edges;
        std::vector<Face> faces;
        std::vector<Vec2> uvs; // Fase 5.1: verbatim lazy array (id-indexed)
        std::vector<ElemId> vSlot, hSlot, eSlot, fSlot;
        std::vector<ElemId> vFree, hFree, eFree, fFree;
        ElemId nextVid = 1, nextHid = 1, nextEid = 1, nextFid = 1;
        size_t aliveV = 0, aliveH = 0, aliveE = 0, aliveF = 0;
    };
    Raw captureRaw() const;
    void restoreRaw(const Raw& r);
    // Exact heap bytes held by this mesh (pools + slot maps + free lists).
    uint64_t rawBytes() const;

private:
    std::vector<Vertex> verts_;
    std::vector<HalfEdge> hes_;
    std::vector<Edge> edges_;
    std::vector<Face> faces_;
    std::vector<ElemId> vSlot_, hSlot_, eSlot_, fSlot_; // id -> slot
    std::vector<ElemId> vFree_, hFree_, eFree_, fFree_; // recycled slots
    ElemId nextVid_ = 1, nextHid_ = 1, nextEid_ = 1, nextFid_ = 1;
    size_t aliveV_ = 0, aliveH_ = 0, aliveE_ = 0, aliveF_ = 0;

    // (origin,dest) -> half-edge id, for O(1) directed half-edge lookup.
    // Derived state: rebuilt by restoreRaw(); kept in sync on create/kill.
    std::unordered_map<uint64_t, ElemId> pairMap_;

    // Per-corner UVs (Fase 5.1): LAZY, indexed by half-edge id, size() is
    // 0 or <= nextHid_. Grown only by the addFace overload with UVs and by
    // splitEdge's interpolation (which never runs while empty).
    std::vector<Vec2> uvs_;

    void killEdge(ElemId edgeId);  // marks edge + its 2 half-edges dead
    void repairOutgoing(ElemId v); // rescan or isolate-kill
    void rebuildPairMap();
};

} // namespace blone
