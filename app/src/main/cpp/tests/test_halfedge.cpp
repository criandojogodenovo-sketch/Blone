// Topology invariants and primitive validity.
#include "test_util.h"

#include "../core/mesh/Primitives.h"

using namespace blone;

namespace {

void testCubeStructure() {
    HalfEdgeMesh m;
    PrimitiveParams p;
    CHECK(buildPrimitive(PrimType::Cube, p, m));
    test::assertInvariants(m, "cubo");
    CHECK(m.aliveVertexCount() == 8);
    CHECK(m.aliveEdgeCount() == 12);
    CHECK(m.aliveFaceCount() == 6);
    // All faces quads; Euler V-E+F = 2.
    for (const ElemId f : m.aliveFaceIds()) {
        CHECK(m.faceSideCount(f) == 4);
    }
    CHECK(m.aliveVertexCount() - m.aliveEdgeCount() + m.aliveFaceCount() == 2);
    // Every face normal points outward from the center (0,1,0).
    for (const ElemId f : m.aliveFaceIds()) {
        const Vec3 n = m.faceNormal(f);
        const Vec3 c = m.faceCentroid(f) - Vec3{0, 1, 0};
        CHECK(dot(n, c) > 0.0f);
    }
}

void testPlaneStructure() {
    HalfEdgeMesh m;
    PrimitiveParams p;
    p.cy = 0.0f;
    CHECK(buildPrimitive(PrimType::Plane, p, m));
    test::assertInvariants(m, "plano");
    CHECK(m.aliveVertexCount() == 4);
    CHECK(m.aliveEdgeCount() == 4);
    CHECK(m.aliveFaceCount() == 1);
    CHECK(m.aliveVertexCount() - m.aliveEdgeCount() + m.aliveFaceCount() == 1);
    // Plane: all 4 edges are boundary, normal +Y.
    for (const ElemId f : m.aliveFaceIds()) {
        const Vec3 n = m.faceNormal(f);
        CHECK_NEAR(n.y, 1.0f, 1e-5f);
    }
    int boundary = 0;
    for (const ElemId e : m.faceEdges(m.aliveFaceIds()[0])) {
        if (m.isBoundaryEdge(e)) ++boundary;
    }
    CHECK(boundary == 4);
}

void testCylinderStructure() {
    HalfEdgeMesh m;
    PrimitiveParams p;
    p.segsU = 8;
    CHECK(buildPrimitive(PrimType::Cylinder, p, m));
    test::assertInvariants(m, "cilindro");
    CHECK(m.aliveVertexCount() == 16);
    CHECK(m.aliveEdgeCount() == 24);
    CHECK(m.aliveFaceCount() == 10); // 8 sides + 2 caps
    CHECK(m.aliveVertexCount() - m.aliveEdgeCount() + m.aliveFaceCount() == 2);
    for (const ElemId f : m.aliveFaceIds()) {
        const Vec3 n = m.faceNormal(f);
        const Vec3 c = m.faceCentroid(f) - Vec3{0, 1, 0};
        CHECK(dot(n, c) > 0.0f); // outward
    }
}

void testSphereStructure() {
    HalfEdgeMesh m;
    PrimitiveParams p;
    p.segsU = 8;
    p.segsV = 4;
    CHECK(buildPrimitive(PrimType::Sphere, p, m));
    test::assertInvariants(m, "esfera");
    CHECK(m.aliveVertexCount() == 2 + 3 * 8); // poles + 3 rings
    CHECK(m.aliveFaceCount() == 2 * 8 + 2 * 8); // caps + middle quads
    CHECK(m.aliveEdgeCount() == 8 * (2 * 4 - 1));
    CHECK(m.aliveVertexCount() - m.aliveEdgeCount() + m.aliveFaceCount() == 2);
    for (const ElemId f : m.aliveFaceIds()) {
        const Vec3 n = m.faceNormal(f);
        const Vec3 c = m.faceCentroid(f) - Vec3{0, 1, 0};
        CHECK(dot(n, c) > 0.0f); // outward everywhere (poles included)
    }
}

void testPrimitiveRejections() {
    HalfEdgeMesh m;
    PrimitiveParams p;
    p.segsU = 2; // below minimum
    CHECK(!buildPrimitive(PrimType::Cylinder, p, m));
    p.segsU = 97; // above maximum
    CHECK(!buildPrimitive(PrimType::Cylinder, p, m));
    p.segsU = 16;
    p.segsV = 1; // below minimum
    CHECK(!buildPrimitive(PrimType::Sphere, p, m));
    p.segsV = 49; // above maximum
    CHECK(!buildPrimitive(PrimType::Sphere, p, m));
    p.segsV = 8;
    p.size = 0.0f; // degenerate size
    CHECK(!buildPrimitive(PrimType::Cube, p, m));
    p.size = 1000.0f; // above maximum
    CHECK(!buildPrimitive(PrimType::Cube, p, m));
    // Failed builds must not have touched the mesh.
    CHECK(m.empty());
}

void testAddFaceRejections() {
    HalfEdgeMesh m;
    PrimitiveParams p;
    CHECK(buildPrimitive(PrimType::Cube, p, m));
    // Duplicate vertices in a loop.
    CHECK(m.addFace({1, 2, 3, 3}) == kNull);
    // Non-manifold: re-use a directed pair that already has a face.
    // Face (1,2,3,4)? not a cube loop; use existing directed he 1->2 (face +Z).
    CHECK(m.addFace({1, 2, 6, 5}) == kNull); // 1->2 already claimed
    test::assertInvariants(m, "cubo após addFace rejeitado");
    // Degenerate: fewer than 3 verts.
    CHECK(m.addFace({1, 2}) == kNull);
}

void testSnapshotRoundTrip() {
    HalfEdgeMesh m;
    PrimitiveParams p;
    CHECK(buildPrimitive(PrimType::Cube, p, m));
    HalfEdgeMesh::Raw raw = m.captureRaw();
    HalfEdgeMesh copy;
    copy.restoreRaw(raw);
    CHECK(test::sameMesh(m, copy));
    test::assertInvariants(copy, "restauro de snapshot");
}

} // namespace

namespace blone::test {
void runHalfEdgeTests() {
    testCubeStructure();
    testPlaneStructure();
    testCylinderStructure();
    testSphereStructure();
    testPrimitiveRejections();
    testAddFaceRejections();
    testSnapshotRoundTrip();
}
} // namespace blone::test
