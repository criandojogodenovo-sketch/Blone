#include "Primitives.h"

#include <cmath>

namespace blone {

namespace {

void buildCube(const PrimitiveParams& p, HalfEdgeMesh& m) {
    const float s = p.size;
    const float x0 = p.cx - s, x1 = p.cx + s;
    const float y0 = p.cy - s, y1 = p.cy + s;
    const float z0 = p.cz - s, z1 = p.cz + s;

    const Vec3 v[8] = {
            {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, // 0..3 front (z+)
            {x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}, // 4..7 back  (z-)
    };
    ElemId id[8];
    for (int i = 0; i < 8; ++i) id[i] = m.addVertex(v[i]);

    // CCW seen from outside (normals verified by tests).
    const int faces[6][4] = {
            {0, 1, 2, 3}, // +Z
            {5, 4, 7, 6}, // -Z
            {1, 5, 6, 2}, // +X
            {4, 0, 3, 7}, // -X
            {3, 2, 6, 7}, // +Y
            {0, 4, 5, 1}, // -Y
    };
    for (const auto& f : faces) {
        m.addFace({id[f[0]], id[f[1]], id[f[2]], id[f[3]]});
    }
}

void buildPlane(const PrimitiveParams& p, HalfEdgeMesh& m) {
    const float s = p.size;
    const Vec3 v[4] = {
            {p.cx - s, p.cy, p.cz + s}, {p.cx + s, p.cy, p.cz + s},
            {p.cx + s, p.cy, p.cz - s}, {p.cx - s, p.cy, p.cz - s},
    };
    ElemId id[4];
    for (int i = 0; i < 4; ++i) id[i] = m.addVertex(v[i]);
    m.addFace({id[0], id[1], id[2], id[3]}); // +Y normal, boundary on all 4 edges
}

void buildCylinder(const PrimitiveParams& p, HalfEdgeMesh& m) {
    const int n = p.segsU;
    const float r = p.size;
    const float yTop = p.cy + r;
    const float yBot = p.cy - r;

    // Vertices 0..n-1: top ring (theta increasing). n..2n-1: bottom ring.
    std::vector<ElemId> topRing(static_cast<size_t>(n));
    std::vector<ElemId> botRing(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float a = static_cast<float>(M_PI) * 2.0f * static_cast<float>(i) / static_cast<float>(n);
        topRing[static_cast<size_t>(i)] =
                m.addVertex({p.cx + r * std::cos(a), yTop, p.cz + r * std::sin(a)});
    }
    for (int i = 0; i < n; ++i) {
        const float a = static_cast<float>(M_PI) * 2.0f * static_cast<float>(i) / static_cast<float>(n);
        botRing[static_cast<size_t>(i)] =
                m.addVertex({p.cx + r * std::cos(a), yBot, p.cz + r * std::sin(a)});
    }

    auto top = [&](int i) { return topRing[static_cast<size_t>(i % n)]; };
    auto bot = [&](int i) { return botRing[static_cast<size_t>(i % n)]; };

    // Side quads (t_i, t_i+1, b_i+1, b_i) — outward radial normals.
    for (int i = 0; i < n; ++i) {
        m.addFace({top(i), top(i + 1), bot(i + 1), bot(i)});
    }
    // Top cap: reversed ring order for +Y normal.
    {
        std::vector<ElemId> cap;
        cap.reserve(static_cast<size_t>(n));
        for (int i = n - 1; i >= 0; --i) cap.push_back(top(i));
        m.addFace(cap);
    }
    // Bottom cap: forward ring order for -Y normal.
    {
        std::vector<ElemId> cap;
        cap.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) cap.push_back(bot(i));
        m.addFace(cap);
    }
}

void buildSphere(const PrimitiveParams& p, HalfEdgeMesh& m) {
    const int su = p.segsU;
    const int sv = p.segsV; // >= 2
    const float r = p.size;

    const ElemId north = m.addVertex({p.cx, p.cy + r, p.cz});
    // Rows j = 1..sv-1, each with su vertices.
    std::vector<ElemId> ringIds(static_cast<size_t>(sv > 1 ? (sv - 1) * su : 0));
    for (int j = 1; j < sv; ++j) {
        const float phi = static_cast<float>(M_PI) * static_cast<float>(j) / static_cast<float>(sv);
        for (int u = 0; u < su; ++u) {
            const float theta = static_cast<float>(M_PI) * 2.0f * static_cast<float>(u) / static_cast<float>(su);
            ringIds[static_cast<size_t>((j - 1) * su + u)] = m.addVertex(
                    {p.cx + r * std::sin(phi) * std::cos(theta), p.cy + r * std::cos(phi),
                     p.cz + r * std::sin(phi) * std::sin(theta)});
        }
    }
    const ElemId south = m.addVertex({p.cx, p.cy - r, p.cz});

    auto ring = [&](int j, int u) -> ElemId {
        return ringIds[static_cast<size_t>((j - 1) * su + (u % su))];
    };

    // North cap triangles (N, v(1,u+1), v(1,u)) — outward.
    for (int u = 0; u < su; ++u) {
        m.addFace({north, ring(1, u + 1), ring(1, u)});
    }
    // Middle quads (v(j,u), v(j,u+1), v(j+1,u+1), v(j+1,u)) — outward.
    for (int j = 1; j < sv - 1; ++j) {
        for (int u = 0; u < su; ++u) {
            m.addFace({ring(j, u), ring(j, u + 1), ring(j + 1, u + 1), ring(j + 1, u)});
        }
    }
    // South cap triangles (S, v(sv-1,u), v(sv-1,u+1)) — outward.
    for (int u = 0; u < su; ++u) {
        m.addFace({south, ring(sv - 1, u), ring(sv - 1, u + 1)});
    }
}

} // namespace

bool buildPrimitive(PrimType type, const PrimitiveParams& p, HalfEdgeMesh& out) {
    if (p.size < kMinSize || p.size > kMaxSize) return false;

    HalfEdgeMesh m;
    switch (type) {
        case PrimType::Cube:
        case PrimType::Plane:
            break;
        case PrimType::Cylinder:
            if (p.segsU < kMinSegmentsU || p.segsU > kMaxSegmentsU) return false;
            break;
        case PrimType::Sphere:
            if (p.segsU < kMinSegmentsU || p.segsU > kMaxSegmentsU) return false;
            if (p.segsV < kMinSegmentsV || p.segsV > kMaxSegmentsV) return false;
            break;
    }

    switch (type) {
        case PrimType::Cube: buildCube(p, m); break;
        case PrimType::Plane: buildPlane(p, m); break;
        case PrimType::Cylinder: buildCylinder(p, m); break;
        case PrimType::Sphere: buildSphere(p, m); break;
    }

    m.relinkBoundaryLoops();
    if (!m.validate() || m.empty()) return false;
    out.restoreRaw(m.captureRaw());
    return true;
}

} // namespace blone
