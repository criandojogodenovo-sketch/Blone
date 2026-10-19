#pragma once
// Minimal test harness for the Blone core (host, no dependencies).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../core/mesh/HalfEdgeMesh.h"

inline int g_failures = 0;
inline int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("FALHOU %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

#define REQUIRE(cond)                                                            \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("FALHOU %s:%d: %s (abortando o teste)\n", __FILE__,      \
                        __LINE__, #cond);                                        \
            return;                                                              \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                    \
    do {                                                                         \
        ++g_checks;                                                              \
        if (std::fabs(static_cast<double>(a) - static_cast<double>(b)) >         \
            static_cast<double>(eps)) {                                          \
            ++g_failures;                                                        \
            std::printf("FALHOU %s:%d: %s = %f vs %s = %f (eps %f)\n", __FILE__, \
                        __LINE__, #a, static_cast<double>(a), #b,                \
                        static_cast<double>(b), static_cast<double>(eps));       \
        }                                                                        \
    } while (0)

namespace blone::test {

// Rebuilds a mesh from a snapshot (comparison helper).
inline HalfEdgeMesh meshFromRaw(const HalfEdgeMesh::Raw& raw) {
    HalfEdgeMesh r;
    r.restoreRaw(raw);
    return r;
}

// Full topology sanity: validate() plus explicit twin/loop sweeps.
inline void assertInvariants(const HalfEdgeMesh& m, const char* where) {
    ++g_checks;
    if (!m.validate()) {
        ++g_failures;
        std::printf("FALHOU invariante topológica em %s\n", where);
        return;
    }
    // I1/I2 explicitly (validate covers them; keep the independent sweep).
    for (const auto& h : m.halfEdges()) {
        if (!h.alive) continue;
        ++g_checks;
        if (m.heg(m.heg(h.id).twin).twin != h.id ||
            m.heg(m.heg(h.id).next).prev != h.id) {
            ++g_failures;
            std::printf("FALHOU twin/next-prev em %s (he %u)\n", where, h.id);
            return;
        }
    }
}

// Byte-exact topology+geometry comparison (used by undo/redo and round-trips).
inline bool sameMesh(const HalfEdgeMesh& a, const HalfEdgeMesh& b) {
    if (a.aliveVertexCount() != b.aliveVertexCount()) return false;
    if (a.aliveEdgeCount() != b.aliveEdgeCount()) return false;
    if (a.aliveFaceCount() != b.aliveFaceCount()) return false;
    if (a.aliveHalfEdgeCount() != b.aliveHalfEdgeCount()) return false;
    for (const auto& v : a.vertices()) {
        if (!v.alive) continue;
        if (!b.vertexAlive(v.id)) return false;
        const auto& w = b.vrt(v.id);
        if (std::fabs(w.pos.x - v.pos.x) > 1e-6f ||
            std::fabs(w.pos.y - v.pos.y) > 1e-6f ||
            std::fabs(w.pos.z - v.pos.z) > 1e-6f) {
            return false;
        }
    }
    for (const auto& f : a.faces()) {
        if (!f.alive) continue;
        if (!b.faceAlive(f.id)) return false;
        if (a.faceVertices(f.id).size() != b.faceVertices(f.id).size()) return false;
        // Same loop contents in the same order.
        auto la = a.faceVertices(f.id);
        auto lb = b.faceVertices(f.id);
        for (size_t i = 0; i < la.size(); ++i) {
            if (la[i] != lb[i]) return false;
        }
    }
    return true;
}

// Finds the undirected edge id between two vertices (0 when absent).
inline ElemId edgeBetween(const HalfEdgeMesh& m, ElemId a, ElemId b) {
    const ElemId h = m.findDirectedHalfEdge(a, b);
    if (h != kNull) return m.heg(h).edge;
    const ElemId h2 = m.findDirectedHalfEdge(b, a);
    if (h2 != kNull) return m.heg(h2).edge;
    return kNull;
}

} // namespace blone::test
