#include "Operations.h"

#include "../Log.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace blone::ops {

namespace {

// Rollback guard: snapshots the mesh, restores on destruction unless committed.
class Rollback {
public:
    explicit Rollback(HalfEdgeMesh& m) : mesh_(m), raw_(m.captureRaw()) {}
    ~Rollback() {
        if (!committed_) mesh_.restoreRaw(raw_);
    }
    Rollback(const Rollback&) = delete;
    Rollback& operator=(const Rollback&) = delete;
    void commit() { committed_ = true; }

private:
    HalfEdgeMesh& mesh_;
    HalfEdgeMesh::Raw raw_;
    bool committed_ = false;
};

inline uint64_t undirectedKey(ElemId a, ElemId b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | b;
}

} // namespace

// ---------------------------------------------------------------- extrude --

Status extrudeFaces(HalfEdgeMesh& m, const std::vector<ElemId>& faces,
                    std::set<ElemId>& capFacesOut) {
    capFacesOut.clear();
    if (faces.empty()) return Status::ErrNoSelection;

    Rollback rb(m);

    std::unordered_set<ElemId> region(faces.begin(), faces.end());
    for (const ElemId f : region) {
        if (!m.faceAlive(f)) return Status::ErrNoSelection;
    }

    // Region edge adjacency + region vertices.
    std::unordered_map<ElemId, int> edgeRegionCount;
    std::unordered_set<ElemId> regionVerts;
    size_t regionSides = 0;
    for (const ElemId f : region) {
        for (const ElemId e : m.faceEdges(f)) ++edgeRegionCount[e];
        for (const ElemId v : m.faceVertices(f)) regionVerts.insert(v);
        regionSides += m.faceSideCount(f);
    }
    std::vector<ElemId> boundaryEdges;
    for (const auto& [e, c] : edgeRegionCount) {
        if (c == 1) boundaryEdges.push_back(e);
    }
    if (boundaryEdges.empty()) {
        return Status::ErrUnsupported; // closed region (e.g. whole cube)
    }

    // Read-only validation: every region boundary loop must close.
    {
        std::unordered_set<ElemId> visited;
        for (const ElemId start : boundaryEdges) {
            if (visited.count(start)) continue;
            ElemId h = m.edg(start).he;
            if (region.count(m.heg(h).face) == 0) h = m.heg(h).twin;
            ElemId curEdge = start;
            size_t steps = 0;
            do {
                visited.insert(curEdge);
                ElemId it = m.heg(h).next;
                size_t guard = 0;
                while (edgeRegionCount[m.heg(it).edge] != 1) {
                    // Interior edge: jump across to the other region face.
                    const ElemId twin = m.heg(it).twin;
                    if (region.count(m.heg(twin).face) == 0) return Status::ErrUnsupported;
                    it = m.heg(twin).next;
                    if (++guard > MeshLimits::maxFaceSides + 2) return Status::ErrUnsupported;
                }
                h = it;
                curEdge = m.heg(h).edge;
                if (++steps > boundaryEdges.size() + 1) return Status::ErrUnsupported;
            } while (curEdge != start);
        }
        if (visited.size() != boundaryEdges.size()) return Status::ErrUnsupported;
    }

    // Limits (conservative upper bounds).
    const size_t newVerts = regionVerts.size();
    const size_t newFaces = region.size() + boundaryEdges.size();
    const size_t newEdgesMax = boundaryEdges.size() * 3 + regionSides;
    if (m.aliveVertexCount() + newVerts > MeshLimits::maxVertices ||
        m.aliveFaceCount() + newFaces > MeshLimits::maxFaces ||
        m.aliveEdgeCount() + newEdgesMax > MeshLimits::maxEdges) {
        return Status::ErrLimit;
    }

    // Remember old positions (region verts can become isolated and die when
    // the region covers a whole mesh boundary) and duplicate ALL region verts.
    std::unordered_map<ElemId, Vec3> oldPos;
    std::unordered_map<ElemId, ElemId> nv;
    oldPos.reserve(regionVerts.size() * 2);
    nv.reserve(regionVerts.size() * 2);
    for (const ElemId v : regionVerts) {
        const Vec3 pos = m.vrt(v).pos; // copy: addVertex may reallocate the pool
        oldPos[v] = pos;
        const ElemId nid = m.addVertex(pos);
        if (nid == kNull) return Status::ErrLimit;
        nv[v] = nid;
    }

    // Capture, for every boundary edge, the region-side direction (a -> b)
    // and the pre-removal face loops, BEFORE the region faces are removed.
    std::unordered_map<ElemId, std::pair<ElemId, ElemId>> rimDir;
    for (const ElemId e : boundaryEdges) {
        ElemId h = m.edg(e).he;
        if (region.count(m.heg(h).face) == 0) h = m.heg(h).twin;
        rimDir[e] = {m.heg(h).origin, m.heg(m.heg(h).next).origin};
    }
    std::vector<std::vector<ElemId>> preLoops;
    preLoops.reserve(region.size());
    for (const ElemId f : region) preLoops.push_back(m.faceVertices(f));

    // Remove the region faces: rim half-edges become boundary, interior region
    // edges die, interior region vertices become isolated and die.
    for (const ElemId f : region) m.removeFaceAndCleanup(f);

    // Recreate (once) region vertices that died with the removed faces so the
    // wall rim stays connected when the region covered a mesh boundary.
    std::unordered_map<ElemId, ElemId> replacement;
    auto getOrRecreate = [&](ElemId v) -> ElemId {
        if (m.vertexAlive(v)) return v;
        auto it = replacement.find(v);
        if (it != replacement.end()) return it->second;
        const ElemId nid = m.addVertex(oldPos[v]);
        replacement[v] = nid;
        return nid;
    };

    // Walls: quad (a, b, nv[b], nv[a]) along every boundary edge, where a->b
    // is the captured region-side direction (CCW around the region).
    for (const ElemId e : boundaryEdges) {
        const auto [a, b] = rimDir[e];
        const ElemId a2 = getOrRecreate(a);
        const ElemId b2 = getOrRecreate(b);
        if (a2 == kNull || b2 == kNull) return Status::ErrLimit;
        const auto ia = nv.find(a);
        const auto ib = nv.find(b);
        if (ia == nv.end() || ib == nv.end()) return Status::ErrInternal;
        if (m.addFace({a2, b2, ib->second, ia->second}) == kNull) return Status::ErrLimit;
    }

    // Caps: replicate every region face loop on the new vertices.
    for (const auto& loop : preLoops) {
        std::vector<ElemId> newLoop;
        newLoop.reserve(loop.size());
        for (const ElemId v : loop) {
            const auto it = nv.find(v);
            if (it == nv.end()) return Status::ErrInternal;
            newLoop.push_back(it->second);
        }
        const ElemId cap = m.addFace(newLoop);
        if (cap == kNull) return Status::ErrLimit;
        capFacesOut.insert(cap);
    }

    m.relinkBoundaryLoops();
    if (!m.validate()) return Status::ErrInternal;
    rb.commit();
    return Status::OK;
}

// -------------------------------------------------------------- subdivide --

Status subdivideFaces(HalfEdgeMesh& m, const std::vector<ElemId>& faces) {
    if (faces.empty()) return Status::ErrNoSelection;
    Rollback rb(m);

    std::vector<ElemId> work;
    work.reserve(faces.size());
    for (const ElemId f : faces) {
        if (!m.faceAlive(f)) return Status::ErrNoSelection;
        const size_t sides = m.faceSideCount(f);
        if (sides != 3 && sides != 4) return Status::ErrUnsupported;
        work.push_back(f);
    }

    // Limits projection: each face -> 4 faces; verts grow by ~edges+faces.
    size_t edgeBudget = 0;
    for (const ElemId f : work) edgeBudget += m.faceSideCount(f);
    if (m.aliveFaceCount() + work.size() * 3 > MeshLimits::maxFaces ||
        m.aliveVertexCount() + edgeBudget + work.size() > MeshLimits::maxVertices ||
        m.aliveEdgeCount() + edgeBudget * 2 > MeshLimits::maxEdges ||
        m.aliveHalfEdgeCount() + edgeBudget * 4 > MeshLimits::maxHalfEdges) {
        return Status::ErrLimit;
    }

    // 1) Split every edge of the selected faces (midpoints shared).
    //    The union of edges is collected BEFORE any split: splitting rewires
    //    neighbour loops and would otherwise present already-split sub-edges
    //    of later faces as new work (double split).
    std::unordered_map<ElemId, ElemId> edgeMid; // old edge id -> mid vertex
    {
        std::set<ElemId> edgesToSplit;
        for (const ElemId f : work) {
            if (!m.faceAlive(f)) return Status::ErrInternal;
            for (const ElemId e : m.faceEdges(f)) edgesToSplit.insert(e);
        }
        for (const ElemId e : edgesToSplit) {
            ElemId mid = kNull, ea = kNull, eb = kNull;
            if (!m.edgeAlive(e)) continue; // split as part of a neighbour? no:
                                           // edges are unique per id and die only
                                           // when split; alive check is defensive
            if (!m.splitEdge(e, mid, ea, eb)) return Status::ErrLimit;
            edgeMid[e] = mid;
        }
    }

    // 2) Divide each face into 4.
    for (const ElemId f : work) {
        if (!m.faceAlive(f)) return Status::ErrInternal;
        const std::vector<ElemId> loop = m.faceVertices(f);
        if (loop.size() < 6 || (loop.size() % 2) != 0) return Status::ErrInternal;
        // Loop alternates corner, mid, corner, mid, ... after the splits.
        Vec3 centroid{};
        int corners = 0;
        for (size_t i = 0; i < loop.size(); i += 2) {
            centroid = centroid + m.vrt(loop[i]).pos;
            ++corners;
        }
        centroid = centroid * (1.0f / static_cast<float>(corners));
        const ElemId ct = m.addVertex(centroid);
        if (ct == kNull) return Status::ErrLimit;

        const bool quad = loop.size() == 8;
        m.removeFaceAndCleanup(f);

        if (quad) {
            const ElemId c0 = loop[0], m01 = loop[1], c1 = loop[2], m12 = loop[3];
            const ElemId c2 = loop[4], m23 = loop[5], c3 = loop[6], m30 = loop[7];
            if (m.addFace({c0, m01, ct, m30}) == kNull) return Status::ErrLimit;
            if (m.addFace({m01, c1, m12, ct}) == kNull) return Status::ErrLimit;
            if (m.addFace({ct, m12, c2, m23}) == kNull) return Status::ErrLimit;
            if (m.addFace({m30, ct, m23, c3}) == kNull) return Status::ErrLimit;
        } else { // triangle
            const ElemId c0 = loop[0], m01 = loop[1], c1 = loop[2];
            const ElemId m12 = loop[3], c2 = loop[4], m20 = loop[5];
            if (m.addFace({c0, m01, m20}) == kNull) return Status::ErrLimit;
            if (m.addFace({m01, c1, m12}) == kNull) return Status::ErrLimit;
            if (m.addFace({m20, m12, c2}) == kNull) return Status::ErrLimit;
            if (m.addFace({m01, m12, m20}) == kNull) return Status::ErrLimit;
        }
    }

    m.relinkBoundaryLoops();
    if (!m.validate()) return Status::ErrInternal;
    rb.commit();
    return Status::OK;
}

// ----------------------------------------------------------------- delete --

Status deleteElements(HalfEdgeMesh& m, DelKind kind, const std::vector<ElemId>& sel) {
    if (sel.empty()) return Status::ErrNoSelection;
    Rollback rb(m);

    if (kind == DelKind::Faces) {
        for (const ElemId f : sel) {
            if (!m.faceAlive(f)) return Status::ErrNoSelection;
        }
        for (const ElemId f : sel) m.removeFaceAndCleanup(f);
    } else if (kind == DelKind::Edges) {
        // Validation pass (read-only) for every selected edge.
        for (const ElemId e : sel) {
            if (!m.edgeAlive(e)) return Status::ErrNoSelection;
            const int fc = m.edgeFaceCount(e);
            if (fc == 2) {
                const ElemId h = m.edg(e).he;
                const ElemId A = m.heg(h).face;
                const ElemId B = m.heg(m.heg(h).twin).face;
                if (A == kNull || B == kNull || A == B) return Status::ErrNonManifold;
                // The two faces must share exactly ONE edge.
                const std::vector<ElemId> edgesOfA = m.faceEdges(A); // copy: two
                std::unordered_set<ElemId> edgesA(edgesOfA.begin(), edgesOfA.end());
                int shared = 0;
                for (const ElemId eb : m.faceEdges(B)) {
                    if (edgesA.count(eb)) ++shared;
                }
                if (shared != 1) return Status::ErrNonManifold;
                const size_t merged = m.faceSideCount(A) + m.faceSideCount(B) - 2;
                if (merged < 3 || merged > MeshLimits::maxFaceSides) {
                    return Status::ErrNonManifold;
                }
            } else if (fc != 1) {
                return Status::ErrNonManifold;
            }
        }
        // Apply sequentially with fresh state per edge.
        for (const ElemId e : sel) {
            if (!m.edgeAlive(e)) continue;
            if (m.edgeFaceCount(e) == 2) {
                ElemId h1 = m.edg(e).he;
                if (m.heg(h1).face == kNull) h1 = m.heg(h1).twin;
                const ElemId h2 = m.heg(h1).twin;
                const ElemId A = m.heg(h1).face;
                const ElemId B = m.heg(h2).face;
                const ElemId n1 = m.heg(h1).next;
                const ElemId p1 = m.heg(h1).prev;
                const ElemId n2 = m.heg(h2).next;
                const ElemId p2 = m.heg(h2).prev;
                // Splice: n1 ... p1 -> n2 ... p2 -> n1
                m.heg(p1).next = n2;
                m.heg(n2).prev = p1;
                m.heg(p2).next = n1;
                m.heg(n1).prev = p2;
                ElemId it = n2;
                size_t guard = 0;
                while (true) {
                    m.heg(it).face = A;
                    if (it == p2) break;
                    it = m.heg(it).next;
                    if (++guard > MeshLimits::maxFaceSides + 2) break;
                }
                m.fac(A).he = n1;
                m.destroyEdgeWithFace(e, B);
            } else {
                // Boundary edge: its single face dies with it.
                const ElemId h = m.edg(e).he;
                const ElemId f = m.heg(h).face != kNull
                                     ? m.heg(h).face
                                     : m.heg(m.heg(h).twin).face;
                if (f != kNull) m.removeFaceAndCleanup(f);
            }
        }
    } else { // Vertices
        // Validation pass: the link of every selected vertex must be a single
        // cycle (interior vertex) or a single path (boundary vertex).
        for (const ElemId v : sel) {
            if (!m.vertexAlive(v)) return Status::ErrNoSelection;
            const std::vector<ElemId> star = m.vertexIncidentFaces(v);
            if (star.empty()) continue; // isolated: removed below
            std::vector<std::pair<ElemId, ElemId>> link;
            std::unordered_set<uint64_t> linkKeys;
            for (const ElemId f : star) {
                const std::vector<ElemId> loop = m.faceVertices(f);
                for (size_t i = 0; i + 1 < loop.size(); ++i) {
                    const ElemId a = loop[i], b = loop[i + 1];
                    if (a == v || b == v) continue;
                    const uint64_t k = undirectedKey(a, b);
                    if (!linkKeys.insert(k).second) return Status::ErrNonManifold;
                    link.emplace_back(a, b);
                }
            }
            std::unordered_map<ElemId, int> degree;
            for (const auto& [a, b] : link) {
                ++degree[a];
                ++degree[b];
            }
            for (const auto& [n, d] : degree) {
                if (d > 2) return Status::ErrNonManifold;
            }
            const size_t nNodes = degree.size();
            if (link.size() != nNodes && link.size() != nNodes - 1) {
                return Status::ErrNonManifold;
            }
            std::unordered_map<ElemId, std::vector<ElemId>> adj;
            for (const auto& [a, b] : link) {
                adj[a].push_back(b);
                adj[b].push_back(a);
            }
            std::unordered_set<ElemId> seen;
            std::vector<ElemId> stack{link.empty() ? kNull : link.front().first};
            while (!stack.empty()) {
                const ElemId cur = stack.back();
                stack.pop_back();
                if (cur == kNull || !seen.insert(cur).second) continue;
                for (const ElemId nb : adj[cur]) stack.push_back(nb);
            }
            if (seen.size() != nNodes) return Status::ErrNonManifold;
        }
        // Apply.
        for (const ElemId v : sel) {
            if (m.vertexAlive(v)) m.removeVertexWithFaces(v);
        }
    }

    m.removeIsolatedElements();
    m.relinkBoundaryLoops();
    if (!m.validate()) return Status::ErrInternal;
    rb.commit();
    return Status::OK;
}

// ---------------------------------------------------------------- loop cut --

Status loopCut(HalfEdgeMesh& m, ElemId edge, LoopCutReport& rep) {
    rep = LoopCutReport{};
    if (!m.edgeAlive(edge)) return Status::ErrNoSelection;

    Rollback rb(m);

    struct Walk {
        bool valid = true;
        bool closed = false;
        std::vector<ElemId> faces;
        std::set<ElemId> crossed; // includes the start edge
    };
    auto walk = [&](ElemId firstFace) -> Walk {
        Walk w;
        ElemId cur = firstFace;
        ElemId entry = edge;
        std::set<ElemId> visited;
        size_t steps = 0;
        while (true) {
            if (++steps > MeshLimits::maxFaces + 2) {
                w.valid = false;
                return w;
            }
            if (m.faceSideCount(cur) != 4) {
                w.valid = false;
                return w;
            }
            if (visited.count(cur)) break; // figure-eight style ring: rejected below
            visited.insert(cur);
            w.faces.push_back(cur);
            w.crossed.insert(entry);
            const ElemId exit = m.oppositeEdgeInQuad(cur, entry);
            if (exit == kNull) {
                w.valid = false;
                return w;
            }
            if (exit == edge) {
                w.crossed.insert(exit);
                w.closed = true;
                break;
            }
            w.crossed.insert(exit);
            if (m.isBoundaryEdge(exit)) break; // open ring terminates here
            const ElemId other = m.edgeOtherFace(exit, cur);
            if (other == kNull) {
                w.valid = false;
                return w;
            }
            cur = other;
            entry = exit;
        }
        return w;
    };

    const ElemId h = m.edg(edge).he;
    ElemId F1 = m.heg(h).face;
    ElemId F2 = m.heg(m.heg(h).twin).face;
    if (F1 == kNull && F2 == kNull) return Status::ErrUnsupported;
    if ((F1 != kNull && m.faceSideCount(F1) != 4) ||
        (F2 != kNull && m.faceSideCount(F2) != 4)) {
        return Status::ErrUnsupported;
    }
    if (F1 == kNull) std::swap(F1, F2); // F1 = first existing face

    Walk wa = walk(F1);
    if (!wa.valid) return Status::ErrUnsupported;
    if (!wa.closed && F2 != kNull) {
        Walk wb = walk(F2);
        if (!wb.valid) return Status::ErrUnsupported;
        for (const ElemId f : wb.faces) {
            if (std::find(wa.faces.begin(), wa.faces.end(), f) != wa.faces.end()) {
                return Status::ErrUnsupported; // overlapping walks
            }
            wa.faces.push_back(f);
        }
        for (const ElemId e : wb.crossed) wa.crossed.insert(e);
    }
    if (wa.faces.empty()) return Status::ErrUnsupported;

    // Final validation of the ring definition.
    const size_t expectCrossed = wa.closed ? wa.faces.size() : wa.faces.size() + 1;
    if (wa.crossed.size() != expectCrossed) return Status::ErrUnsupported;

    // Limits.
    if (m.aliveVertexCount() + wa.crossed.size() > MeshLimits::maxVertices ||
        m.aliveEdgeCount() + wa.crossed.size() + wa.faces.size() > MeshLimits::maxEdges ||
        m.aliveFaceCount() + wa.faces.size() > MeshLimits::maxFaces ||
        m.aliveHalfEdgeCount() + wa.crossed.size() * 2 + wa.faces.size() * 2 >
                MeshLimits::maxHalfEdges) {
        return Status::ErrLimit;
    }

    // 1) Split every crossed edge.
    std::unordered_map<ElemId, ElemId> midOf; // old crossed edge -> mid vertex
    for (const ElemId e : wa.crossed) {
        if (!m.edgeAlive(e)) return Status::ErrInternal;
        ElemId mid = kNull, ea = kNull, eb = kNull;
        if (!m.splitEdge(e, mid, ea, eb)) return Status::ErrLimit;
        midOf[e] = mid;
        rep.newVertices.push_back(mid);
    }

    // 2) Divide each ring face into two quads between the two crossed mids.
    //    The loop is read AFTER all splits: a quad whose two crossed
    //    (opposite) edges were split now has 6 half-edges
    //    [c0, m1, c1, c2, m2, c3].
    //    Two passes: (a) remove ALL ring faces first — this can kill edges
    //    and vertices when a ring face was the only face using them (e.g. a
    //    single-quad plane); (b) rebuild each pair of quads, recreating any
    //    dead loop vertex ONCE at its recorded position.
    std::vector<std::pair<ElemId, std::vector<ElemId>>> loopsPerFace;
    loopsPerFace.reserve(wa.faces.size());
    std::unordered_map<ElemId, Vec3> loopPos;
    for (const ElemId fid : wa.faces) {
        if (!m.faceAlive(fid)) return Status::ErrInternal;
        const std::vector<ElemId> loop = m.faceVertices(fid);
        if (loop.size() != 6) return Status::ErrInternal;
        loopsPerFace.emplace_back(fid, loop);
        for (const ElemId v : loop) loopPos[v] = m.vrt(v).pos;
    }
    for (const ElemId fid : wa.faces) m.removeFaceAndCleanup(fid);

    std::unordered_map<ElemId, ElemId> recreated;
    auto resolveVert = [&](ElemId v) -> ElemId {
        if (m.vertexAlive(v)) return v;
        auto it = recreated.find(v);
        if (it != recreated.end()) return it->second;
        const ElemId nid = m.addVertex(loopPos[v]);
        recreated[v] = nid;
        return nid;
    };

    for (const auto& [fid, loop] : loopsPerFace) {
        int i1 = -1, i2 = -1;
        for (size_t i = 0; i < 6; ++i) {
            bool isMid = false;
            for (const auto& [e, mid] : midOf) {
                if (mid == loop[i]) {
                    isMid = true;
                    break;
                }
            }
            if (isMid) {
                if (i1 < 0) i1 = static_cast<int>(i);
                else i2 = static_cast<int>(i);
            }
        }
        if (i1 < 0 || i2 < 0 || (i2 - i1) != 3) return Status::ErrInternal;
        ElemId x[6];
        for (int k = 0; k < 6; ++k) x[k] = loop[(i1 - 1 + k + 6) % 6];
        // x = [c0, m1, c1, c2, m2, c3]
        ElemId r[6];
        for (int k = 0; k < 6; ++k) {
            r[k] = resolveVert(x[k]);
            if (r[k] == kNull) return Status::ErrLimit;
        }
        if (m.addFace({r[0], r[1], r[4], r[5]}) == kNull) return Status::ErrLimit;
        if (m.addFace({r[1], r[2], r[3], r[4]}) == kNull) return Status::ErrLimit;
        const ElemId loopHe = m.findDirectedHalfEdge(r[1], r[4]);
        if (loopHe == kNull) return Status::ErrInternal;
        rep.newLoopEdges.push_back(m.heg(loopHe).edge);
        rep.ringFaces.push_back(fid); // old id of the divided face (report)
    }

    for (const ElemId e : wa.crossed) rep.crossedEdges.push_back(e);

    m.relinkBoundaryLoops();
    if (!m.validate()) return Status::ErrInternal;
    rb.commit();
    return Status::OK;
}

} // namespace blone::ops
