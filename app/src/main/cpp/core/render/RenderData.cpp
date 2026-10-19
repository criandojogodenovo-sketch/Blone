#include "RenderData.h"

#include <cmath>

namespace blone::renderdata {

namespace {

// 2D point used by the ear-clipping projection.
struct P2 {
    float x = 0.0f;
    float y = 0.0f;
};

float cross2(const P2& o, const P2& a, const P2& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

bool pointInTriangle(const P2& p, const P2& a, const P2& b, const P2& c) {
    const float d1 = cross2(a, b, p);
    const float d2 = cross2(b, c, p);
    const float d3 = cross2(c, a, p);
    // Points exactly on an edge count as inside (conservative clipping).
    const bool hasNeg = (d1 < -1e-9f) || (d2 < -1e-9f) || (d3 < -1e-9f);
    const bool hasPos = (d1 > 1e-9f) || (d2 > 1e-9f) || (d3 > 1e-9f);
    return !(hasNeg && hasPos);
}

} // namespace

void faceTriangles(const HalfEdgeMesh& m, ElemId f, std::vector<ElemId>& out) {
    const std::vector<ElemId> loop = m.faceVertices(f);
    const size_t n = loop.size();
    if (n < 3) return;

    // Quad contract (pinned by tests): [a,b,c,d] -> (a,b,c),(a,c,d).
    // The fan and ear clipping agree exactly on convex/quad loops.
    if (n == 3) {
        out.push_back(loop[0]);
        out.push_back(loop[1]);
        out.push_back(loop[2]);
        return;
    }
    if (n == 4) {
        out.push_back(loop[0]);
        out.push_back(loop[1]);
        out.push_back(loop[2]);
        out.push_back(loop[0]);
        out.push_back(loop[2]);
        out.push_back(loop[3]);
        return;
    }

    // Ear clipping for n >= 5 (concave-safe: no bowtie triangles).
    const Vec3 nrm = m.faceNormal(f);
    if (length(nrm) < 0.5f) { // degenerate normal: fan fallback
        for (size_t i = 1; i + 1 < n; ++i) {
            out.push_back(loop[0]);
            out.push_back(loop[i]);
            out.push_back(loop[i + 1]);
        }
        return;
    }

    // Project onto the dominant plane of the normal.
    const float ax = std::fabs(nrm.x), ay = std::fabs(nrm.y), az = std::fabs(nrm.z);
    std::vector<P2> pts(n);
    for (size_t i = 0; i < n; ++i) {
        const Vec3& p = m.vrt(loop[i]).pos;
        if (ax >= ay && ax >= az) {
            pts[i] = {p.y, p.z};
        } else if (ay >= az) {
            pts[i] = {p.z, p.x};
        } else {
            pts[i] = {p.x, p.y};
        }
    }
    // Ensure the projected loop is CCW (positive area) so ear tests are valid.
    float area2 = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const P2& a = pts[i];
        const P2& b = pts[(i + 1) % n];
        area2 += a.x * b.y - b.x * a.y;
    }
    if (area2 < 0.0f) {
        for (P2& p : pts) p = {p.y, p.x}; // mirror flips orientation
    }

    std::vector<size_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = i;
    size_t guard = 0;
    const size_t guardMax = n * n + 16;
    size_t i = 0;
    while (idx.size() > 3 && guard < guardMax) {
        const size_t count = idx.size();
        const size_t i0 = idx[(i + 0) % count];
        const size_t i1 = idx[(i + 1) % count];
        const size_t i2 = idx[(i + 2) % count];
        const P2& a = pts[i0];
        const P2& b = pts[i1];
        const P2& c = pts[i2];
        bool clipped = false;
        if (cross2(a, b, c) > 1e-9f) { // convex corner
            bool contains = false;
            for (size_t k = 0; k < count && !contains; ++k) {
                if (k == (i + 0) % count || k == (i + 1) % count ||
                    k == (i + 2) % count) {
                    continue;
                }
                if (pointInTriangle(pts[idx[k]], a, b, c)) contains = true;
            }
            if (!contains) {
                out.push_back(loop[i0]);
                out.push_back(loop[i1]);
                out.push_back(loop[i2]);
                idx.erase(idx.begin() + static_cast<long>((i + 1) % count));
                clipped = true;
                if (i > 0) --i;
            }
        }
        if (!clipped) ++i;
        ++guard;
    }
    if (idx.size() == 3) {
        out.push_back(loop[idx[0]]);
        out.push_back(loop[idx[1]]);
        out.push_back(loop[idx[2]]);
        return;
    }
    // Fallback (should not happen for simple loops): fan from vertex 0.
    for (size_t k = 1; k + 1 < n; ++k) {
        out.push_back(loop[0]);
        out.push_back(loop[k]);
        out.push_back(loop[k + 1]);
    }
}

void buildObject(const SceneObject& obj, RenderData& out) {
    out = RenderData{};
    const HalfEdgeMesh& mesh = obj.mesh;

    const size_t faceGuess = mesh.aliveFaceCount();
    out.fillPos.reserve(faceGuess * 4 * 3);
    out.fillNrm.reserve(faceGuess * 4 * 3);
    out.fillIdx.reserve(faceGuess * 6);
    out.wirePos.reserve(mesh.aliveEdgeCount() * 2 * 3);
    out.pointPos.reserve(mesh.aliveVertexCount() * 3);

    std::vector<ElemId> tris;
    for (const ElemId f : mesh.aliveFaceIds()) {
        const std::vector<ElemId> loop = mesh.faceVertices(f);
        if (loop.size() < 3) continue;
        const Vec3 nrm = mesh.faceNormal(f);
        const uint32_t base = static_cast<uint32_t>(out.fillVertexCount);
        for (const ElemId v : loop) {
            const Vec3& p = mesh.vrt(v).pos;
            out.fillPos.push_back(p.x);
            out.fillPos.push_back(p.y);
            out.fillPos.push_back(p.z);
            out.fillNrm.push_back(nrm.x);
            out.fillNrm.push_back(nrm.y);
            out.fillNrm.push_back(nrm.z);
            ++out.fillVertexCount;
        }
        tris.clear();
        faceTriangles(mesh, f, tris);
        for (const ElemId t : tris) {
            // Local index of the vertex inside this face's duplicated block.
            size_t local = 0;
            for (size_t k = 0; k < loop.size(); ++k) {
                if (loop[k] == t) {
                    local = k;
                    break;
                }
            }
            out.fillIdx.push_back(base + static_cast<uint32_t>(local));
        }
    }
    out.triangleCount = out.fillIdx.size() / 3;

    for (const auto& e : mesh.edges()) {
        if (!e.alive) continue;
        const ElemId h = e.he;
        const Vec3& a = mesh.vrt(mesh.heg(h).origin).pos;
        const Vec3& b = mesh.vrt(mesh.heg(mesh.heg(h).twin).origin).pos;
        out.wirePos.push_back(a.x);
        out.wirePos.push_back(a.y);
        out.wirePos.push_back(a.z);
        out.wirePos.push_back(b.x);
        out.wirePos.push_back(b.y);
        out.wirePos.push_back(b.z);
    }

    for (const auto& v : mesh.vertices()) {
        if (!v.alive) continue;
        out.pointPos.push_back(v.pos.x);
        out.pointPos.push_back(v.pos.y);
        out.pointPos.push_back(v.pos.z);
        ++out.editableVertexCount;
    }

    out.bounds = mesh.computeBounds();
}

void buildSelection(const SceneObject& obj, SelectionData& out) {
    out = SelectionData{};
    const HalfEdgeMesh& mesh = obj.mesh;

    std::vector<ElemId> tris;
    for (const ElemId f : obj.sel.faces) {
        if (!mesh.faceAlive(f)) continue;
        const std::vector<ElemId> loop = mesh.faceVertices(f);
        if (loop.size() < 3) continue;
        const Vec3 nrm = mesh.faceNormal(f);
        const uint32_t base = static_cast<uint32_t>(out.facePos.size() / 3);
        for (const ElemId v : loop) {
            const Vec3& p = mesh.vrt(v).pos;
            out.facePos.push_back(p.x);
            out.facePos.push_back(p.y);
            out.facePos.push_back(p.z);
            out.faceNrm.push_back(nrm.x);
            out.faceNrm.push_back(nrm.y);
            out.faceNrm.push_back(nrm.z);
        }
        tris.clear();
        faceTriangles(mesh, f, tris);
        for (const ElemId t : tris) {
            size_t local = 0;
            for (size_t k = 0; k < loop.size(); ++k) {
                if (loop[k] == t) {
                    local = k;
                    break;
                }
            }
            out.faceIdx.push_back(base + static_cast<uint32_t>(local));
        }
    }

    auto pushEdge = [&](ElemId e) {
        if (!mesh.edgeAlive(e)) return;
        const ElemId h = mesh.edg(e).he;
        const Vec3& a = mesh.vrt(mesh.heg(h).origin).pos;
        const Vec3& b = mesh.vrt(mesh.heg(mesh.heg(h).twin).origin).pos;
        out.edgePos.push_back(a.x);
        out.edgePos.push_back(a.y);
        out.edgePos.push_back(a.z);
        out.edgePos.push_back(b.x);
        out.edgePos.push_back(b.y);
        out.edgePos.push_back(b.z);
    };
    for (const ElemId e : obj.sel.edges) pushEdge(e);
    for (const ElemId f : obj.sel.faces) {
        if (!mesh.faceAlive(f)) continue;
        for (const ElemId e : mesh.faceEdges(f)) pushEdge(e);
    }

    for (const ElemId v : obj.sel.verts) {
        if (!mesh.vertexAlive(v)) continue;
        const Vec3& p = mesh.vrt(v).pos;
        out.vertPos.push_back(p.x);
        out.vertPos.push_back(p.y);
        out.vertPos.push_back(p.z);
    }
}

} // namespace blone::renderdata
