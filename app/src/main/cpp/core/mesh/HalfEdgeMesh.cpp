#include "HalfEdgeMesh.h"

#include "../Log.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace blone {

namespace {
inline uint64_t pairKey(ElemId a, ElemId b) {
    return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
}
constexpr size_t kMaxLoopWalk = 1u << 22; // hard bound against corrupted loops
} // namespace

// ---------------------------------------------------------------- slots ---

bool HalfEdgeMesh::vertexAlive(ElemId id) const {
    return id != kNull && id < vSlot_.size() && verts_[vSlot_[id]].alive &&
           verts_[vSlot_[id]].id == id;
}
bool HalfEdgeMesh::halfEdgeAlive(ElemId id) const {
    return id != kNull && id < hSlot_.size() && hes_[hSlot_[id]].alive &&
           hes_[hSlot_[id]].id == id;
}
bool HalfEdgeMesh::edgeAlive(ElemId id) const {
    return id != kNull && id < eSlot_.size() && edges_[eSlot_[id]].alive &&
           edges_[eSlot_[id]].id == id;
}
bool HalfEdgeMesh::faceAlive(ElemId id) const {
    return id != kNull && id < fSlot_.size() && faces_[fSlot_[id]].alive &&
           faces_[fSlot_[id]].id == id;
}

ElemId HalfEdgeMesh::addVertex(const Vec3& p) {
    if (aliveV_ >= MeshLimits::maxVertices) return kNull;
    ElemId slot;
    if (!vFree_.empty()) {
        slot = vFree_.back();
        vFree_.pop_back();
    } else {
        slot = verts_.size();
        verts_.emplace_back();
    }
    Vertex& v = verts_[slot];
    v.id = nextVid_++;
    v.pos = p;
    v.outgoing = kNull;
    v.alive = true;
    if (vSlot_.size() <= v.id) vSlot_.resize(static_cast<size_t>(v.id) + 1);
    vSlot_[v.id] = slot;
    ++aliveV_;
    return v.id;
}

ElemId HalfEdgeMesh::findDirectedHalfEdge(ElemId a, ElemId b) const {
    auto it = pairMap_.find(pairKey(a, b));
    if (it == pairMap_.end()) return kNull;
    return halfEdgeAlive(it->second) ? it->second : kNull;
}

ElemId HalfEdgeMesh::addFace(const std::vector<ElemId>& loop) {
    // No-UV path (every Fase 2-4 caller): delegates with an EMPTY uv list —
    // the lazy array is never grown, so meshes without UV data keep paying
    // zero bytes and behave exactly as before Fase 5.1.
    return addFace(loop, std::vector<Vec2>{});
}

ElemId HalfEdgeMesh::addFace(const std::vector<ElemId>& loop,
                             const std::vector<Vec2>& cornerUvs) {
    const size_t n = loop.size();
    if (n < 3 || n > MeshLimits::maxFaceSides) return kNull;
    if (aliveF_ >= MeshLimits::maxFaces) return kNull;
    // Fase 5.1: a non-empty uv list must be EXACTLY parallel to the loop.
    // Checked up front with every other rejection — the call fails clean.
    if (!cornerUvs.empty() && cornerUvs.size() != n) return kNull;

    // Validate vertices: alive and distinct.
    for (size_t i = 0; i < n; ++i) {
        if (!vertexAlive(loop[i])) return kNull;
        for (size_t j = i + 1; j < n; ++j) {
            if (loop[i] == loop[j]) return kNull;
        }
    }

    // Pass 1: resolve every directed pair. Claim free boundary half-edges,
    // mark missing ones for creation. Read-only: abort leaves no mutation.
    std::vector<ElemId> claimed(n, kNull); // existing boundary he reused
    size_t toCreate = 0;
    for (size_t i = 0; i < n; ++i) {
        const ElemId a = loop[i];
        const ElemId b = loop[(i + 1) % n];
        const ElemId he = findDirectedHalfEdge(a, b);
        if (he != kNull) {
            if (heg(he).face != kNull) {
                // Both sides already used -> non-manifold request.
                BLONE_LOGW("addFace: pair %u->%u already has a face", a, b);
                return kNull;
            }
            claimed[i] = he;
        } else {
            ++toCreate;
        }
    }
    if (aliveH_ + toCreate * 2 > MeshLimits::maxHalfEdges) return kNull;
    if (aliveE_ + toCreate > MeshLimits::maxEdges) return kNull;

    // Pass 2: create the face and the missing half-edge pairs.
    const ElemId fid = nextFid_++;
    ElemId slot;
    if (!fFree_.empty()) {
        slot = fFree_.back();
        fFree_.pop_back();
    } else {
        slot = faces_.size();
        faces_.emplace_back();
    }
    Face& f = faces_[slot];
    f.id = fid;
    f.he = kNull;
    f.alive = true;
    if (fSlot_.size() <= fid) fSlot_.resize(static_cast<size_t>(fid) + 1);
    fSlot_[fid] = slot;
    ++aliveF_;

    for (size_t i = 0; i < n; ++i) {
        if (claimed[i] != kNull) continue;
        const ElemId a = loop[i];
        const ElemId b = loop[(i + 1) % n];
        // Two half-edges + one edge; the twin stays boundary for now.
        ElemId heSlot;
        if (!hFree_.empty()) {
            heSlot = hFree_.back();
            hFree_.pop_back();
        } else {
            heSlot = hes_.size();
            hes_.emplace_back();
        }
        ElemId twSlot;
        if (!hFree_.empty()) {
            twSlot = hFree_.back();
            hFree_.pop_back();
        } else {
            twSlot = hes_.size();
            hes_.emplace_back();
        }
        ElemId eSlot;
        if (!eFree_.empty()) {
            eSlot = eFree_.back();
            eFree_.pop_back();
        } else {
            eSlot = edges_.size();
            edges_.emplace_back();
        }

        HalfEdge& h = hes_[heSlot];
        h.id = nextHid_++;
        h.origin = a;
        h.twin = kNull; // fixed below
        h.next = kNull;
        h.prev = kNull;
        h.face = kNull; // fixed below
        h.edge = kNull;
        h.alive = true;

        HalfEdge& t = hes_[twSlot];
        t.id = nextHid_++;
        t.origin = b;
        t.twin = kNull;
        t.next = kNull;
        t.prev = kNull;
        t.face = kNull;
        t.edge = kNull;
        t.alive = true;

        h.twin = t.id;
        t.twin = h.id;

        Edge& e = edges_[eSlot];
        e.id = nextEid_++;
        e.he = h.id;
        e.alive = true;
        if (eSlot_.size() <= e.id) eSlot_.resize(static_cast<size_t>(e.id) + 1);
        eSlot_[e.id] = eSlot;

        h.edge = e.id;
        t.edge = e.id;

        if (hSlot_.size() <= h.id) hSlot_.resize(static_cast<size_t>(h.id) + 1);
        hSlot_[h.id] = heSlot;
        if (hSlot_.size() <= t.id) hSlot_.resize(static_cast<size_t>(t.id) + 1);
        hSlot_[t.id] = twSlot;

        pairMap_[pairKey(a, b)] = h.id;
        pairMap_[pairKey(b, a)] = t.id;

        aliveH_ += 2;
        ++aliveE_;

        claimed[i] = h.id;
    }

    // Fase 5.1 (uv pass): every face-side half-edge id is final now —
    // claimed[i] holds the corner carrier of loop[i] (created or reused).
    // Grow the lazy array ONCE to cover the new ids (created boundary twins
    // default to (0,0)) and copy the corner UVs in.
    if (!cornerUvs.empty()) {
        if (uvs_.size() < static_cast<size_t>(nextHid_)) {
            uvs_.resize(static_cast<size_t>(nextHid_));
        }
        for (size_t i = 0; i < n; ++i) uvs_[claimed[i]] = cornerUvs[i];
    }

    // Pass 3: assign the face, link the loop, fix vertex.outgoing.
    for (size_t i = 0; i < n; ++i) {
        HalfEdge& h = heg(claimed[i]);
        h.face = fid;
        HalfEdge& nxt = heg(claimed[(i + 1) % n]);
        h.next = nxt.id;
        nxt.prev = h.id;
        Vertex& vo = vrt(h.origin);
        if (vo.outgoing == kNull || !halfEdgeAlive(vo.outgoing)) {
            vo.outgoing = h.id;
        }
    }
    fac(fid).he = claimed[0];
    return fid;
}

// ------------------------------------------------------------- splitEdge ---

bool HalfEdgeMesh::splitEdge(ElemId edgeId, ElemId& midVert, ElemId& edgeA,
                             ElemId& edgeB) {
    if (!edgeAlive(edgeId)) return false;
    Edge& e = edg(edgeId);
    ElemId h1 = e.he, h2 = heg(h1).twin;
    if (heg(h1).face == kNull) std::swap(h1, h2); // h1 is the face side
    if (heg(h1).face == kNull) return false;      // no face at all: broken

    const ElemId a = heg(h1).origin;
    const ElemId b = heg(h2).origin;
    const ElemId F1 = heg(h1).face;
    const ElemId F2 = heg(h2).face; // may be kNull (boundary)

    const ElemId n1 = heg(h1).next; // starts at b
    const ElemId n2 = heg(h2).next; // starts at a
    const ElemId p2 = heg(h2).prev; // ends at b

    // Limits: +1 vertex, +2 edges, +2 half-edges (3 created, 1 dies).
    if (aliveV_ + 1 > MeshLimits::maxVertices) return false;
    if (aliveE_ + 2 > MeshLimits::maxEdges) return false;
    if (aliveH_ + 2 > MeshLimits::maxHalfEdges) return false;

    const Vec3 midPos = (vrt(a).pos + vrt(b).pos) * 0.5f;
    const ElemId m = addVertex(midPos);
    if (m == kNull) return false;

    auto allocHe = [&]() -> ElemId {
        ElemId slot;
        if (!hFree_.empty()) {
            slot = hFree_.back();
            hFree_.pop_back();
        } else {
            slot = hes_.size();
            hes_.emplace_back();
        }
        HalfEdge& h = hes_[slot];
        h.id = nextHid_++;
        h.alive = true;
        h.next = h.prev = h.twin = h.face = h.edge = h.origin = kNull;
        if (hSlot_.size() <= h.id) hSlot_.resize(static_cast<size_t>(h.id) + 1);
        hSlot_[h.id] = slot;
        ++aliveH_;
        return h.id;
    };
    auto allocEdge = [&]() -> ElemId {
        ElemId slot;
        if (!eFree_.empty()) {
            slot = eFree_.back();
            eFree_.pop_back();
        } else {
            slot = edges_.size();
            edges_.emplace_back();
        }
        Edge& ed = edges_[slot];
        ed.id = nextEid_++;
        ed.he = kNull;
        ed.alive = true;
        if (eSlot_.size() <= ed.id) eSlot_.resize(static_cast<size_t>(ed.id) + 1);
        eSlot_[ed.id] = slot;
        ++aliveE_;
        return ed.id;
    };

    const ElemId hMA = allocHe(); // m -> a
    const ElemId hMB = allocHe(); // m -> b
    const ElemId hBA = allocHe(); // b -> m
    const ElemId e1 = allocEdge(); // (a->m, m->a)
    const ElemId e2 = allocEdge(); // (m->b, b->m)

    // E1 = (h1, hMA); E2 = (hMB, hBA)
    heg(h1).edge = e1;
    heg(hMA).edge = e1;
    heg(hMB).edge = e2;
    heg(hBA).edge = e2;
    edg(e1).he = h1;
    edg(e2).he = hMB;

    heg(h1).twin = hMA;
    heg(hMA).twin = h1;
    heg(hMB).twin = hBA;
    heg(hBA).twin = hMB;

    heg(hMA).origin = m;
    heg(hMB).origin = m;
    heg(hBA).origin = b;

    heg(hMA).face = F2; // kNull when boundary
    heg(hMB).face = F1;
    heg(hBA).face = F2; // kNull when boundary

    // F1 side: h1 (a->m) -> hMB (m->b) -> n1
    heg(h1).next = hMB;
    heg(hMB).prev = h1;
    heg(hMB).next = n1;
    heg(n1).prev = hMB;

    // F2 / boundary side: p2 -> hBA (b->m) -> hMA (m->a) -> n2
    heg(p2).next = hBA;
    heg(hBA).prev = p2;
    heg(hBA).next = hMA;
    heg(hMA).prev = hBA;
    heg(hMA).next = n2;
    heg(n2).prev = hMA;

    // Face handles: h2 belonged to F2.
    if (F2 != kNull && fac(F2).he == h2) fac(F2).he = hBA;
    // h1 was possibly the face handle of F1 — still alive, nothing to do.

    // Kill h2 and the old edge MANUALLY: killEdge() would follow the (already
    // rewired) twin chain of h1 and kill the wrong half-edge. h1 survives as
    // the a->m side of e1.
    pairMap_.erase(pairKey(a, b));  // old directed pairs
    pairMap_.erase(pairKey(b, a));
    heg(h2).alive = false;
    hFree_.push_back(hSlot_[heg(h2).id]);
    --aliveH_;
    edg(edgeId).alive = false;
    eFree_.push_back(eSlot_[edg(edgeId).id]);
    --aliveE_;
    // New directed pairs.
    pairMap_[pairKey(a, m)] = h1;
    pairMap_[pairKey(m, a)] = hMA;
    pairMap_[pairKey(m, b)] = hMB;
    pairMap_[pairKey(b, m)] = hBA;

    // Repair outgoing for a, b, m.
    repairOutgoing(a);
    repairOutgoing(b);
    if (vrt(m).outgoing == kNull) vrt(m).outgoing = hMB;

    // Fase 5.1: keep the per-corner uv array parallel on this topological
    // split (data maintenance only — the rewiring above is untouched).
    // Runs ONLY when the mesh already carries uv data. F2's corner-at-b uv
    // moves from the dying h2 to hBA (same corner, new half-edge); the two
    // new mid corners get the midpoint of their face's edge endpoints —
    // the standard subdivide rule, so uv continuity survives the split.
    if (!uvs_.empty()) {
        const Vec2 f1a = uv(h1); // F1 corner at a (h1 survives)
        const Vec2 f1b = uv(n1); // F1 corner at b (n1 survives)
        const Vec2 f2b = uv(h2); // F2 corner at b (h2 dies -> hBA)
        const Vec2 f2a = uv(n2); // F2 corner at a (n2 survives)
        if (uvs_.size() < static_cast<size_t>(nextHid_)) {
            uvs_.resize(static_cast<size_t>(nextHid_));
        }
        uvs_[hBA] = f2b;
        uvs_[hMB] = Vec2{(f1a.x + f1b.x) * 0.5f, (f1a.y + f1b.y) * 0.5f};
        uvs_[hMA] = Vec2{(f2b.x + f2a.x) * 0.5f, (f2b.y + f2a.y) * 0.5f};
    }

    midVert = m;
    edgeA = e1;
    edgeB = e2;
    return true;
}

// -------------------------------------------------------------- deletion ---

void HalfEdgeMesh::killEdge(ElemId edgeId) {
    if (!edgeAlive(edgeId)) return;
    const ElemId h = edg(edgeId).he;
    const ElemId t = heg(h).twin;
    auto erasePair = [&](ElemId heId) {
        if (!halfEdgeAlive(heId)) return;
        const ElemId o = heg(heId).origin;
        const ElemId d = heg(heg(heId).next).origin;
        pairMap_.erase(pairKey(o, d));
    };
    erasePair(h);
    erasePair(t);
    if (halfEdgeAlive(h)) {
        heg(h).alive = false;
        hFree_.push_back(hSlot_[h]);
        --aliveH_;
    }
    if (halfEdgeAlive(t)) {
        heg(t).alive = false;
        hFree_.push_back(hSlot_[t]);
        --aliveH_;
    }
    edg(edgeId).alive = false;
    eFree_.push_back(eSlot_[edg(edgeId).id]);
    --aliveE_;
}

void HalfEdgeMesh::repairOutgoing(ElemId v) {
    if (!vertexAlive(v)) return;
    Vertex& vert = vrt(v);
    if (vert.outgoing != kNull && halfEdgeAlive(vert.outgoing) &&
        heg(vert.outgoing).origin == v) {
        return;
    }
    for (auto& h : hes_) {
        if (h.alive && h.origin == v) {
            vert.outgoing = h.id;
            return;
        }
    }
    // Isolated vertex: remove it.
    vert.alive = false;
    vFree_.push_back(vSlot_[vert.id]);
    --aliveV_;
}

void HalfEdgeMesh::relinkBoundaryLoops() {
    // In a manifold mesh every vertex has at most one outgoing boundary
    // half-edge. Rebuild next/prev among boundary half-edges from scratch:
    //   next(h) = the boundary half-edge starting at the destination of h.
    // The destination of h is the OTHER vertex of its edge = origin of its
    // twin. If some vertex ends up with two outgoing boundary half-edges
    // (non-manifold vertex) the relink is arbitrary and validate() will
    // reject it, letting the caller roll back from its snapshot.
    std::unordered_map<ElemId, ElemId> boundaryFrom;
    for (const auto& h : hes_) {
        if (h.alive && h.face == kNull) boundaryFrom[h.origin] = h.id;
    }
    for (auto& h : hes_) {
        if (!h.alive || h.face != kNull) continue;
        const ElemId other = heg(h.twin).origin; // destination of h
        auto it = boundaryFrom.find(other);
        h.next = (it != boundaryFrom.end()) ? it->second : kNull;
    }
    for (auto& h : hes_) {
        if (!h.alive || h.face != kNull) continue;
        const ElemId nx = h.next;
        if (nx != kNull) heg(nx).prev = h.id;
        else h.prev = kNull;
    }
}

void HalfEdgeMesh::removeFaceAndCleanup(ElemId f) {
    if (!faceAlive(f)) return;
    const std::vector<ElemId> loop = faceHalfEdges(f);
    std::vector<ElemId> affectedEdges;
    std::vector<ElemId> affectedVerts;
    affectedEdges.reserve(loop.size());
    affectedVerts.reserve(loop.size());
    for (const ElemId h : loop) {
        heg(h).face = kNull;
        affectedEdges.push_back(heg(h).edge);
        affectedVerts.push_back(heg(h).origin);
    }
    for (const ElemId e : affectedEdges) {
        if (!edgeAlive(e)) continue;
        const ElemId h = edg(e).he;
        const ElemId t = heg(h).twin;
        const bool hFree = !halfEdgeAlive(h) || heg(h).face == kNull;
        const bool tFree = !halfEdgeAlive(t) || heg(t).face == kNull;
        if (hFree && tFree) killEdge(e);
    }
    fac(f).alive = false;
    fFree_.push_back(fSlot_[fac(f).id]);
    --aliveF_;

    for (const ElemId v : affectedVerts) repairOutgoing(v);
    // Freed half-edges (twins of dead edges' face side) must be relinked
    // into the boundary loops.
    relinkBoundaryLoops();
}

void HalfEdgeMesh::removeVertexWithFaces(ElemId v) {
    if (!vertexAlive(v)) return;
    const std::vector<ElemId> faces = vertexIncidentFaces(v);
    for (const ElemId f : faces) removeFaceAndCleanup(f);
    if (vertexAlive(v)) {
        // All incident half-edges died with the faces.
        repairOutgoing(v); // isolates and removes the vertex
    }
}

void HalfEdgeMesh::removeIsolatedElements() {
    // Vertices are handled by repairOutgoing; here we only sweep defensively.
    for (auto& v : verts_) {
        if (v.alive) repairOutgoing(v.id);
    }
}

void HalfEdgeMesh::destroyEdgeWithFace(ElemId edgeId, ElemId faceId) {
    if (!edgeAlive(edgeId)) return;
    const ElemId h = edg(edgeId).he;
    const ElemId t = heg(h).twin;
    const ElemId a = heg(h).origin;
    const ElemId b = heg(t).origin;
    // Kill the face (free list + counter).
    if (faceAlive(faceId)) {
        fac(faceId).alive = false;
        fFree_.push_back(fSlot_[fac(faceId).id]);
        --aliveF_;
    }
    // Kill both half-edges + the edge via killEdge (still consistent: the
    // caller relinked the surviving loop before calling this).
    killEdge(edgeId);
    repairOutgoing(a);
    repairOutgoing(b);
}

// --------------------------------------------------------------- queries ---

std::vector<ElemId> HalfEdgeMesh::faceHalfEdges(ElemId f) const {
    std::vector<ElemId> out;
    if (!faceAlive(f)) return out;
    const ElemId start = fac(f).he;
    ElemId h = start;
    size_t guard = 0;
    do {
        out.push_back(h);
        h = heg(h).next;
        if (++guard > kMaxLoopWalk) return {};
    } while (h != start && h != kNull);
    return out;
}

std::vector<ElemId> HalfEdgeMesh::faceVertices(ElemId f) const {
    std::vector<ElemId> out;
    if (!faceAlive(f)) return out;
    const ElemId start = fac(f).he;
    ElemId h = start;
    size_t guard = 0;
    do {
        out.push_back(heg(h).origin);
        h = heg(h).next;
        if (++guard > kMaxLoopWalk) return {};
    } while (h != start && h != kNull);
    return out;
}

std::vector<ElemId> HalfEdgeMesh::faceEdges(ElemId f) const {
    std::vector<ElemId> out;
    if (!faceAlive(f)) return out;
    const ElemId start = fac(f).he;
    ElemId h = start;
    size_t guard = 0;
    do {
        out.push_back(heg(h).edge);
        h = heg(h).next;
        if (++guard > kMaxLoopWalk) return {};
    } while (h != start && h != kNull);
    return out;
}

size_t HalfEdgeMesh::faceSideCount(ElemId f) const {
    return faceVertices(f).size();
}

std::vector<ElemId> HalfEdgeMesh::vertexIncidentFaces(ElemId v) const {
    std::vector<ElemId> out;
    if (!vertexAlive(v)) return out;
    for (const auto& f : faces_) {
        if (!f.alive) continue;
        const ElemId start = f.he;
        ElemId h = start;
        size_t guard = 0;
        bool found = false;
        do {
            if (heg(h).origin == v) {
                found = true;
                break;
            }
            h = heg(h).next;
            if (++guard > kMaxLoopWalk) break;
        } while (h != start && h != kNull);
        if (found) out.push_back(f.id);
    }
    return out;
}

std::vector<ElemId> HalfEdgeMesh::vertexNeighbors(ElemId v) const {
    std::vector<ElemId> out;
    if (!vertexAlive(v)) return out;
    for (const auto& f : faces_) {
        if (!f.alive) continue;
        const ElemId start = f.he;
        ElemId h = start;
        size_t guard = 0;
        do {
            if (heg(h).origin == v) {
                // v's outgoing he in this face: neighbor = dest(h); and the
                // he ending at v gives the other neighbor.
                const ElemId dest = heg(h).next; // he starting at dest
                out.push_back(heg(dest).origin);
                const ElemId prevHe = heg(h).prev;
                out.push_back(heg(prevHe).origin);
                break;
            }
            h = heg(h).next;
            if (++guard > kMaxLoopWalk) break;
        } while (h != start && h != kNull);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    out.erase(std::remove(out.begin(), out.end(), v), out.end());
    return out;
}

int HalfEdgeMesh::edgeFaceCount(ElemId e) const {
    if (!edgeAlive(e)) return 0;
    const ElemId h = edg(e).he;
    const ElemId t = heg(h).twin;
    int c = 0;
    if (halfEdgeAlive(h) && heg(h).face != kNull) ++c;
    if (halfEdgeAlive(t) && heg(t).face != kNull) ++c;
    return c;
}

bool HalfEdgeMesh::isBoundaryEdge(ElemId e) const {
    return edgeAlive(e) && edgeFaceCount(e) == 1;
}

ElemId HalfEdgeMesh::edgeOtherFace(ElemId e, ElemId f) const {
    if (!edgeAlive(e)) return kNull;
    const ElemId h = edg(e).he;
    const ElemId t = heg(h).twin;
    if (halfEdgeAlive(h) && heg(h).face != kNull && heg(h).face != f) return heg(h).face;
    if (halfEdgeAlive(t) && heg(t).face != kNull && heg(t).face != f) return heg(t).face;
    return kNull;
}

ElemId HalfEdgeMesh::oppositeEdgeInQuad(ElemId face, ElemId edge) const {
    if (!faceAlive(face) || !edgeAlive(edge)) return kNull;
    const std::vector<ElemId> edges = faceEdges(face);
    if (edges.size() != 4) return kNull;
    for (size_t i = 0; i < 4; ++i) {
        if (edges[i] == edge) return edges[(i + 2) % 4];
    }
    return kNull;
}

Vec3 HalfEdgeMesh::faceNormal(ElemId f) const {
    const std::vector<ElemId> verts = faceVertices(f);
    Vec3 n{};
    if (verts.size() < 3) return n;
    // Newell's method — robust for non-planar and concave polygons.
    for (size_t i = 0; i < verts.size(); ++i) {
        const Vec3& cur = vrt(verts[i]).pos;
        const Vec3& nxt = vrt(verts[(i + 1) % verts.size()]).pos;
        n.x += (cur.y - nxt.y) * (cur.z + nxt.z);
        n.y += (cur.z - nxt.z) * (cur.x + nxt.x);
        n.z += (cur.x - nxt.x) * (cur.y + nxt.y);
    }
    return normalize(n);
}

Vec3 HalfEdgeMesh::faceCentroid(ElemId f) const {
    const std::vector<ElemId> verts = faceVertices(f);
    Vec3 c{};
    if (verts.empty()) return c;
    for (const ElemId v : verts) c = c + vrt(v).pos;
    return c * (1.0f / static_cast<float>(verts.size()));
}

Aabb HalfEdgeMesh::computeBounds() const {
    Aabb b{0, 0, 0, 0, 0, 0};
    bool first = true;
    for (const auto& v : verts_) {
        if (!v.alive) continue;
        if (first) {
            b = {v.pos.x, v.pos.y, v.pos.z, v.pos.x, v.pos.y, v.pos.z};
            first = false;
        } else {
            b.minX = std::min(b.minX, v.pos.x);
            b.minY = std::min(b.minY, v.pos.y);
            b.minZ = std::min(b.minZ, v.pos.z);
            b.maxX = std::max(b.maxX, v.pos.x);
            b.maxY = std::max(b.maxY, v.pos.y);
            b.maxZ = std::max(b.maxZ, v.pos.z);
        }
    }
    return b;
}

std::vector<ElemId> HalfEdgeMesh::aliveFaceIds() const {
    std::vector<ElemId> out;
    out.reserve(aliveF_);
    for (const auto& f : faces_) {
        if (f.alive) out.push_back(f.id);
    }
    return out;
}

bool HalfEdgeMesh::isBoundaryVertex(ElemId v) const {
    if (!vertexAlive(v)) return false;
    for (const ElemId f : vertexIncidentFaces(v)) {
        for (const ElemId e : faceEdges(f)) {
            if (!edgeAlive(e) || !isBoundaryEdge(e)) continue;
            const ElemId h = edg(e).he;
            const ElemId t = heg(h).twin;
            if (heg(h).origin == v || heg(t).origin == v) return true;
        }
    }
    return false;
}

// ------------------------------------------------------------ validation ---


bool HalfEdgeMesh::validate() const {
    if (aliveV_ > MeshLimits::maxVertices || aliveH_ > MeshLimits::maxHalfEdges ||
        aliveE_ > MeshLimits::maxEdges || aliveF_ > MeshLimits::maxFaces) {
        BLONE_LOGE("validate: limits exceeded");
        return false;
    }
    // I8 (Fase 5.1): the lazy per-corner uv array never leaves the
    // half-edge id space (ids are monotonic; entries are id-indexed).
    if (uvs_.size() > static_cast<size_t>(nextHid_)) {
        BLONE_LOGE("validate: uv array exceeds the half-edge id space");
        return false;
    }
    // I1, I2 (local), I5 for half-edges.
    for (const auto& h : hes_) {
        if (!h.alive) continue;
        if (!halfEdgeAlive(h.twin)) return false;
        if (heg(h.twin).twin != h.id) return false;                              // I1
        if (!halfEdgeAlive(h.next) || !halfEdgeAlive(h.prev)) return false;      // I5
        if (heg(h.next).prev != h.id || heg(h.prev).next != h.id) return false;  // I2
        if (!vertexAlive(h.origin)) return false;                                // I5
        if (!edgeAlive(h.edge)) return false;
        if (edg(h.edge).he != h.id && heg(edg(h.edge).he).twin != h.id) return false;
        if (h.face != kNull && !faceAlive(h.face)) return false;
    }
    // I3, I4, I6 for edges.
    for (const auto& e : edges_) {
        if (!e.alive) continue;
        if (!halfEdgeAlive(e.he)) return false;
        const ElemId h = e.he;
        const ElemId t = heg(h).twin;
        if (heg(h).edge != e.id || heg(t).edge != e.id) return false;
        const int fc = (heg(h).face != kNull ? 1 : 0) + (heg(t).face != kNull ? 1 : 0);
        if (fc < 1 || fc > 2) return false;                                      // I4
        if (fc == 2 && heg(h).face == heg(t).face) return false;                 // no fin
    }
    // I3, I6, I7 for faces + loop closure.
    for (const auto& f : faces_) {
        if (!f.alive) continue;
        if (!halfEdgeAlive(f.he) || heg(f.he).face != f.id) return false;        // I3/I6
        const ElemId start = f.he;
        ElemId h = heg(start).next;
        size_t count = 1;
        while (h != start) {
            if (h == kNull || !halfEdgeAlive(h)) return false;
            if (heg(h).face != f.id) return false;                               // I3
            ++count;
            if (count > MeshLimits::maxFaceSides) return false;                  // I7
            h = heg(h).next;
        }
        if (count < 3) return false;                                             // I7
        // Distinct vertices in the loop.
        std::vector<ElemId> verts;
        verts.reserve(count);
        h = start;
        do {
            verts.push_back(heg(h).origin);
            h = heg(h).next;
        } while (h != start);
        std::sort(verts.begin(), verts.end());
        if (std::adjacent_find(verts.begin(), verts.end()) != verts.end()) return false;
    }
    // Boundary half-edges: their next/prev must also be boundary half-edges
    // and each must lie on a closed cycle (bounded walk).
    for (const auto& h : hes_) {
        if (!h.alive || h.face != kNull) continue;
        if (heg(h.next).face != kNull || heg(h.prev).face != kNull) return false;
        ElemId it = h.next;
        size_t guard = 0;
        while (it != h.id) {
            if (it == kNull || !halfEdgeAlive(it) || heg(it).face != kNull) return false;
            if (++guard > aliveH_ + 1) return false; // not a simple cycle
            it = heg(it).next;
        }
    }
    // I6 for vertices.
    for (const auto& v : verts_) {
        if (!v.alive) continue;
        if (v.outgoing == kNull || !halfEdgeAlive(v.outgoing) ||
            heg(v.outgoing).origin != v.id) {
            return false;
        }
    }
    // Alive counters consistency.
    size_t cv = 0, ch = 0, ce = 0, cf = 0;
    for (const auto& v : verts_) cv += v.alive ? 1 : 0;
    for (const auto& h : hes_) ch += h.alive ? 1 : 0;
    for (const auto& e : edges_) ce += e.alive ? 1 : 0;
    for (const auto& f : faces_) cf += f.alive ? 1 : 0;
    if (cv != aliveV_ || ch != aliveH_ || ce != aliveE_ || cf != aliveF_) return false;
    return true;
}

// ---------------------------------------------------------- snapshot ---

HalfEdgeMesh::Raw HalfEdgeMesh::captureRaw() const {
    Raw r;
    r.verts = verts_;
    r.hes = hes_;
    r.edges = edges_;
    r.faces = faces_;
    r.uvs = uvs_; // Fase 5.1: verbatim lazy array (empty == no uv data)
    r.vSlot = vSlot_;
    r.hSlot = hSlot_;
    r.eSlot = eSlot_;
    r.fSlot = fSlot_;
    r.vFree = vFree_;
    r.hFree = hFree_;
    r.eFree = eFree_;
    r.fFree = fFree_;
    r.nextVid = nextVid_;
    r.nextHid = nextHid_;
    r.nextEid = nextEid_;
    r.nextFid = nextFid_;
    r.aliveV = aliveV_;
    r.aliveH = aliveH_;
    r.aliveE = aliveE_;
    r.aliveF = aliveF_;
    return r;
}

void HalfEdgeMesh::restoreRaw(const Raw& r) {
    verts_ = r.verts;
    hes_ = r.hes;
    edges_ = r.edges;
    faces_ = r.faces;
    // Fase 5.1: verbatim — never resized here. A Raw from a v1/v2/v3
    // project file (or any pre-uv snapshot) carries an EMPTY array, which
    // keeps the mesh exactly as frugal as before; uv() then answers the
    // (0,0) default for every corner.
    uvs_ = r.uvs;
    vSlot_ = r.vSlot;
    hSlot_ = r.hSlot;
    eSlot_ = r.eSlot;
    fSlot_ = r.fSlot;
    vFree_ = r.vFree;
    hFree_ = r.hFree;
    eFree_ = r.eFree;
    fFree_ = r.fFree;
    nextVid_ = r.nextVid;
    nextHid_ = r.nextHid;
    nextEid_ = r.nextEid;
    nextFid_ = r.nextFid;
    aliveV_ = r.aliveV;
    aliveH_ = r.aliveH;
    aliveE_ = r.aliveE;
    aliveF_ = r.aliveF;
    rebuildPairMap();
}

void HalfEdgeMesh::rebuildPairMap() {
    pairMap_.clear();
    for (const auto& h : hes_) {
        if (!h.alive) continue;
        const ElemId d = heg(h.next).origin;
        pairMap_[pairKey(h.origin, d)] = h.id;
    }
}

uint64_t HalfEdgeMesh::rawBytes() const {
    auto bytes = [](const auto& v) {
        return static_cast<uint64_t>(v.capacity()) * sizeof(typename std::decay_t<decltype(v)>::value_type);
    };
    uint64_t total = 0;
    total += bytes(verts_) + bytes(hes_) + bytes(edges_) + bytes(faces_);
    total += bytes(uvs_); // Fase 5.1 (0 for meshes without uv data)
    total += bytes(vSlot_) + bytes(hSlot_) + bytes(eSlot_) + bytes(fSlot_);
    total += bytes(vFree_) + bytes(hFree_) + bytes(eFree_) + bytes(fFree_);
    // Pair map: ~2 nodes worth of overhead per entry (conservative estimate).
    total += static_cast<uint64_t>(pairMap_.size()) * 32u;
    return total;
}

} // namespace blone
