// Extrude, subdivide and delete — real geometry, validated topology.
#include "test_util.h"

#include "../core/operations/Operations.h"
#include "../core/mesh/Primitives.h"

using namespace blone;

namespace {

struct CubeIds {
    HalfEdgeMesh m;
    ElemId v[8] = {};
    ElemId fTop = 0, fBottom = 0, fFront = 0, fBack = 0, fRight = 0, fLeft = 0;

    CubeIds() {
        PrimitiveParams p;
        p.cy = 0.0f; // centered at the origin for the pinned coordinates
        CHECK(buildPrimitive(PrimType::Cube, p, m));
        for (const auto& vtx : m.vertices()) {
            if (!vtx.alive) continue;
            if (vtx.pos.x < 0 && vtx.pos.y < 0 && vtx.pos.z > 0) v[0] = vtx.id;
            if (vtx.pos.x > 0 && vtx.pos.y < 0 && vtx.pos.z > 0) v[1] = vtx.id;
            if (vtx.pos.x > 0 && vtx.pos.y > 0 && vtx.pos.z > 0) v[2] = vtx.id;
            if (vtx.pos.x < 0 && vtx.pos.y > 0 && vtx.pos.z > 0) v[3] = vtx.id;
            if (vtx.pos.x < 0 && vtx.pos.y < 0 && vtx.pos.z < 0) v[4] = vtx.id;
            if (vtx.pos.x > 0 && vtx.pos.y < 0 && vtx.pos.z < 0) v[5] = vtx.id;
            if (vtx.pos.x > 0 && vtx.pos.y > 0 && vtx.pos.z < 0) v[6] = vtx.id;
            if (vtx.pos.x < 0 && vtx.pos.y > 0 && vtx.pos.z < 0) v[7] = vtx.id;
        }
        for (const ElemId f : m.aliveFaceIds()) {
            const Vec3 n = m.faceNormal(f);
            if (n.y > 0.5f) fTop = f;
            if (n.y < -0.5f) fBottom = f;
            if (n.z > 0.5f) fFront = f;
            if (n.z < -0.5f) fBack = f;
            if (n.x > 0.5f) fRight = f;
            if (n.x < -0.5f) fLeft = f;
        }
    }
};

void testExtrudeCubeTopFace() {
    CubeIds c;
    const HalfEdgeMesh::Raw before = c.m.captureRaw();

    std::set<ElemId> caps;
    const ops::Status st = ops::extrudeFaces(c.m, {c.fTop}, caps);
    CHECK(st == ops::Status::OK);
    test::assertInvariants(c.m, "extrusão face do cubo");

    // Exact structural result (derived and pinned):
    // V 8->12 (4 duplicated rim verts), E 12->20, F 6->10 (4 walls + 1 cap).
    CHECK(c.m.aliveVertexCount() == 12);
    CHECK(c.m.aliveEdgeCount() == 20);
    CHECK(c.m.aliveFaceCount() == 10);
    CHECK(c.m.aliveVertexCount() - c.m.aliveEdgeCount() + c.m.aliveFaceCount() == 2);
    CHECK(caps.size() == 1);
    CHECK(c.m.faceAlive(*caps.begin()));
    CHECK(!c.m.faceAlive(c.fTop)); // original face replaced by the cap

    // Extruding must create NEW geometry, not just move the face: the walls
    // span from the original rim (old vertices) up to the cap (new vertices
    // at the same positions before any move).
    const ElemId cap = *caps.begin();
    for (const ElemId v : c.m.faceVertices(cap)) {
        const Vec3& p = c.m.vrt(v).pos;
        CHECK_NEAR(std::fabs(p.y) - 1.0f, 0.0f, 1e-5f); // cap at y = +1
    }
    // Old rim vertices still exist at y = +1 (bottom rim of the walls).
    int rimVerts = 0;
    for (const auto& vtx : c.m.vertices()) {
        if (vtx.alive && std::fabs(vtx.pos.y - 1.0f) < 1e-5f) ++rimVerts;
    }
    CHECK(rimVerts == 8); // 4 old + 4 duplicated

    // Move the cap up by 1 (walls are zero-area at extrusion time, so their
    // normals only become meaningful after the cap moves).
    for (const ElemId v : c.m.faceVertices(cap)) {
        c.m.vrt(v).pos.y += 1.0f;
    }
    for (const ElemId f : c.m.aliveFaceIds()) {
        const Vec3 n = c.m.faceNormal(f);
        if (std::fabs(n.y) > 0.5f) continue; // cap (up) and rim (down)
        const Vec3 cen = c.m.faceCentroid(f);
        CHECK(c.m.faceSideCount(f) == 4);             // walls are quads
        CHECK(dot(Vec3{cen.x, 0, cen.z}, Vec3{n.x, 0, n.z}) > 0.0f); // outward
    }

    // Extruding the whole cube (closed region) is rejected untouched.
    CubeIds c2;
    std::set<ElemId> caps2;
    const auto all = c2.m.aliveFaceIds();
    const ops::Status st2 = ops::extrudeFaces(c2.m, all, caps2);
    CHECK(st2 == ops::Status::ErrUnsupported);
    CHECK(test::sameMesh(c2.m, test::meshFromRaw(before)));
}

void testExtrudePlaneRegion() {
    // Extruding the single face of a plane (region covering the whole mesh
    // boundary) must also work: walls + cap, boundary ring recreated.
    HalfEdgeMesh m;
    PrimitiveParams p;
    p.cy = 0.0f;
    CHECK(buildPrimitive(PrimType::Plane, p, m));
    const ElemId f = m.aliveFaceIds()[0];
    std::set<ElemId> caps;
    const ops::Status st = ops::extrudeFaces(m, {f}, caps);
    CHECK(st == ops::Status::OK);
    test::assertInvariants(m, "extrusão do plano");
    // V: 4 old + 4 new = 8; F: 4 walls + 1 cap = 5; E: 4 vertical + 4 cap + 4 rim = 12.
    CHECK(m.aliveVertexCount() == 8);
    CHECK(m.aliveEdgeCount() == 12);
    CHECK(m.aliveFaceCount() == 5);
    CHECK(m.aliveVertexCount() - m.aliveEdgeCount() + m.aliveFaceCount() == 1);
}

void testSubdivideWholeCube() {
    CubeIds c;
    const auto all = c.m.aliveFaceIds();
    const ops::Status st = ops::subdivideFaces(c.m, all);
    CHECK(st == ops::Status::OK);
    test::assertInvariants(c.m, "subdivisão do cubo");
    // V 8->26 (12 mids + 6 centroids), E 12->48, F 6->24; Euler = 2.
    CHECK(c.m.aliveVertexCount() == 26);
    CHECK(c.m.aliveEdgeCount() == 48);
    CHECK(c.m.aliveFaceCount() == 24);
    CHECK(c.m.aliveVertexCount() - c.m.aliveEdgeCount() + c.m.aliveFaceCount() == 2);
    // Shape preserved: all original corners still at their positions.
    const Vec3 corner[8] = {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1},
                            {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}};
    for (const Vec3& e : corner) {
        bool found = false;
        for (const auto& vtx : c.m.vertices()) {
            if (vtx.alive && std::fabs(vtx.pos.x - e.x) < 1e-5f &&
                std::fabs(vtx.pos.y - e.y) < 1e-5f && std::fabs(vtx.pos.z - e.z) < 1e-5f) {
                found = true;
                break;
            }
        }
        CHECK(found);
    }
    // All faces are still quads.
    for (const ElemId f : c.m.aliveFaceIds()) {
        CHECK(c.m.faceSideCount(f) == 4);
    }
}

void testSubdivideSingleFace() {
    CubeIds c;
    const ops::Status st = ops::subdivideFaces(c.m, {c.fTop});
    CHECK(st == ops::Status::OK);
    test::assertInvariants(c.m, "subdivisão da face do topo");
    // V 8->13 (4 mids + 1 centroid), E 12->20, F 6->9.
    CHECK(c.m.aliveVertexCount() == 13);
    CHECK(c.m.aliveEdgeCount() == 20);
    CHECK(c.m.aliveFaceCount() == 9);
    CHECK(c.m.aliveVertexCount() - c.m.aliveEdgeCount() + c.m.aliveFaceCount() == 2);
    // Neighbours of the subdivided face keep their shape: they become
    // pentagons (documented behaviour), the untouched opposite stays a quad.
    int pentagons = 0, quads = 0;
    for (const ElemId f : c.m.aliveFaceIds()) {
        const size_t s = c.m.faceSideCount(f);
        if (s == 5) ++pentagons;
        if (s == 4) ++quads;
    }
    CHECK(pentagons == 4);
    CHECK(quads == 5); // 4 new sub-quads + the untouched bottom
}

void testDeleteFaceEdgeVertex() {
    // Delete face: V/E unchanged minus cleanups; Euler consistent.
    {
        CubeIds c;
        const ops::Status st = ops::deleteElements(c.m, ops::DelKind::Faces, {c.fTop});
        CHECK(st == ops::Status::OK);
        test::assertInvariants(c.m, "apagar face");
        CHECK(c.m.aliveFaceCount() == 5);
        CHECK(c.m.aliveVertexCount() == 8);
        CHECK(c.m.aliveEdgeCount() == 12);
        CHECK(c.m.aliveVertexCount() - c.m.aliveEdgeCount() + c.m.aliveFaceCount() == 1);
    }
    // Delete interior edge: the two faces merge into a hexagon.
    {
        CubeIds c;
        const ElemId e = test::edgeBetween(c.m, c.v[0], c.v[3]); // top rim edge
        CHECK(e != kNull);
        CHECK(c.m.edgeFaceCount(e) == 2);
        const ops::Status st = ops::deleteElements(c.m, ops::DelKind::Edges, {e});
        CHECK(st == ops::Status::OK);
        test::assertInvariants(c.m, "apagar aresta interior");
        CHECK(c.m.aliveVertexCount() == 8);
        CHECK(c.m.aliveEdgeCount() == 11);
        CHECK(c.m.aliveFaceCount() == 5);
        // One face is now a hexagon.
        int hex = 0;
        for (const ElemId f : c.m.aliveFaceIds()) {
            if (c.m.faceSideCount(f) == 6) ++hex;
        }
        CHECK(hex == 1);
        CHECK(c.m.aliveVertexCount() - c.m.aliveEdgeCount() + c.m.aliveFaceCount() == 2);
    }
    // Delete vertex (interior): star faces removed, hexagonal hole left.
    {
        CubeIds c;
        const ops::Status st = ops::deleteElements(c.m, ops::DelKind::Vertices, {c.v[0]});
        CHECK(st == ops::Status::OK);
        test::assertInvariants(c.m, "apagar vértice interior");
        CHECK(c.m.aliveVertexCount() == 7);
        CHECK(c.m.aliveFaceCount() == 3);
        CHECK(c.m.aliveEdgeCount() == 9);
        CHECK(c.m.aliveVertexCount() - c.m.aliveEdgeCount() + c.m.aliveFaceCount() == 1);
    }
    // Delete boundary edge of a plane: face dies with it, mesh becomes empty.
    {
        HalfEdgeMesh m;
        PrimitiveParams p;
        p.cy = 0.0f;
        CHECK(buildPrimitive(PrimType::Plane, p, m));
        const ElemId e = test::edgeBetween(m, 1, 2);
        const ops::Status st = ops::deleteElements(m, ops::DelKind::Edges, {e});
        CHECK(st == ops::Status::OK);
        test::assertInvariants(m, "apagar aresta de fronteira do plano");
        CHECK(m.empty());
    }
}

void testDeleteValidationRejections() {
    // Non-manifold vertex: two quads touching in a single vertex ("bowtie").
    {
        HalfEdgeMesh m;
        for (const Vec3& p : {Vec3{-2, 0, 1}, Vec3{-1, 0, 1}, Vec3{-1, 0, -1}, Vec3{-2, 0, -1},
                              Vec3{1, 0, 1}, Vec3{2, 0, 1}, Vec3{2, 0, -1}, Vec3{1, 0, -1}}) {
            CHECK(m.addVertex(p) != kNull);
        }
        // Share vertex 2: quad A = (1,2,3,4), quad B = (5,2,6,7)? B must not
        // reuse A's directed pairs; use (6,5,8,7) passing through... simplest:
        // two quads sharing ONLY vertex 2.
        CHECK(m.addFace({1, 2, 3, 4}) != kNull);
        CHECK(m.addFace({5, 2, 6, 7}) != kNull);
        // NOTE: no assertInvariants here — the two quads touch in ONE vertex,
        // which is vertex-non-manifold: that vertex has two outgoing boundary
        // half-edges and boundary loops cannot be linked consistently. The
        // delete operation below must still reject it via the link check.
        const HalfEdgeMesh::Raw before = m.captureRaw();
        const ops::Status st =
                ops::deleteElements(m, ops::DelKind::Vertices, {2});
        CHECK(st == ops::Status::ErrNonManifold);
        CHECK(test::sameMesh(m, test::meshFromRaw(before))); // mesh intact
    }
    // Edge whose two faces share more than one edge: a two-sided plane (the
    // same quad added twice with opposite windings shares ALL 4 edge ids).
    // The merge must be rejected and the mesh kept intact.
    {
        HalfEdgeMesh m;
        for (const Vec3& p : {Vec3{-1, 0, 1}, Vec3{1, 0, 1}, Vec3{1, 0, -1}, Vec3{-1, 0, -1}}) {
            CHECK(m.addVertex(p) != kNull);
        }
        CHECK(m.addFace({1, 2, 3, 4}) != kNull);
        CHECK(m.addFace({4, 3, 2, 1}) != kNull); // opposite winding, same edges
        m.relinkBoundaryLoops(); // manual build
        test::assertInvariants(m, "plano de duas faces");
        const ElemId e = test::edgeBetween(m, 1, 2);
        CHECK(e != kNull);
        CHECK(m.edgeFaceCount(e) == 2);
        const HalfEdgeMesh::Raw before = m.captureRaw();
        const ops::Status st = ops::deleteElements(m, ops::DelKind::Edges, {e});
        CHECK(st == ops::Status::ErrNonManifold);
        CHECK(test::sameMesh(m, test::meshFromRaw(before)));
    }
    // Loop cut rejection is covered in test_loopcut.cpp.
}

} // namespace

namespace blone::test {
void runOpsTests() {
    testExtrudeCubeTopFace();
    testExtrudePlaneRegion();
    testSubdivideWholeCube();
    testSubdivideSingleFace();
    testDeleteFaceEdgeVertex();
    testDeleteValidationRejections();
}
} // namespace blone::test
