// Loop cut — the concrete, pinned definition (Phase 2 condition 1).
//
// Mesh under test: the Blone cube primitive:
//   verts (ids 1..8):  v1(-1,-1, 1) v2(1,-1, 1) v3(1, 1, 1) v4(-1, 1, 1)
//                      v5(-1,-1,-1) v6(1,-1,-1) v7(1, 1,-1) v8(-1, 1,-1)
//   faces: +Z (1,2,3,4)  -Z (6,5,8,7)  +X (2,6,7,3)
//          -X (5,1,4,8)  +Y (4,3,7,8)  -Y (1,5,6,2)
//
// Cut through the edge (v1,v4) [shared by +Z and -X]:
//   ring walk: +Z -> (opposite edge (v2,v3)) -> +X -> (v6,v7) -> -Z -> (v5,v8)
//   -> -X -> back to (v1,v4): CLOSED ring of 4 faces.
//   Crossed edges (split): (v1,v4), (v2,v3), (v6,v7), (v5,v8)  -> 4 mids.
//   New loop edges: 4 (one per ring face).
//   Expected result: V 8->12, E 12->20, F 6->10 (4 ring faces become 8 quads;
//   +Y and -Y gain 2 loop vertices each and become hexagons), Euler = 2,
//   new vertices exactly at the midpoints, all invariants hold.
#include "test_util.h"

#include "../core/operations/Operations.h"
#include "../core/mesh/Primitives.h"

using namespace blone;

namespace {

struct CubeRef {
    HalfEdgeMesh m;
    ElemId v[8] = {};
    ElemId fPlusZ = 0, fMinusZ = 0, fPlusX = 0, fMinusX = 0, fPlusY = 0, fMinusY = 0;

    explicit CubeRef() {
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
            if (n.z > 0.5f) fPlusZ = f;
            if (n.z < -0.5f) fMinusZ = f;
            if (n.x > 0.5f) fPlusX = f;
            if (n.x < -0.5f) fMinusX = f;
            if (n.y > 0.5f) fPlusY = f;
            if (n.y < -0.5f) fMinusY = f;
        }
    }
};

void testLoopCutCubeClosedRing() {
    CubeRef c;
    const ElemId e14 = test::edgeBetween(c.m, c.v[0], c.v[3]); // (v1,v4)
    CHECK(e14 != kNull);

    ops::LoopCutReport rep;
    const ops::Status st = ops::loopCut(c.m, e14, rep);
    CHECK(st == ops::Status::OK);
    test::assertInvariants(c.m, "loop cut cubo");

    // Exact structural results derived above.
    CHECK(c.m.aliveVertexCount() == 12);
    CHECK(c.m.aliveEdgeCount() == 20);
    CHECK(c.m.aliveFaceCount() == 10);
    CHECK(c.m.aliveVertexCount() - c.m.aliveEdgeCount() + c.m.aliveFaceCount() == 2);

    // The ring faces (+Z, +X, -Z, -X) were each divided into 2 quads.
    CHECK(rep.ringFaces.size() == 4);
    CHECK(rep.crossedEdges.size() == 4);
    CHECK(rep.newVertices.size() == 4);
    CHECK(rep.newLoopEdges.size() == 4);
    for (const ElemId f : rep.ringFaces) {
        CHECK(!c.m.faceAlive(f)); // originals replaced by their two halves
    }
    for (const ElemId e : rep.crossedEdges) {
        CHECK(!c.m.edgeAlive(e)); // crossed edges were split (old ids retired)
    }

    // New vertices sit exactly at the midpoints of the crossed edges.
    // Crossed: (v1,v4), (v2,v3), (v6,v7), (v5,v8) — all at y = 0.
    const Vec3 expectMid[4] = {
            {-1.0f, 0.0f, 1.0f},  // (v1,v4)
            {1.0f, 0.0f, 1.0f},   // (v2,v3)
            {1.0f, 0.0f, -1.0f},  // (v6,v7)
            {-1.0f, 0.0f, -1.0f}, // (v5,v8)
    };
    int matched = 0;
    for (const ElemId nv : rep.newVertices) {
        const Vec3 p = c.m.vrt(nv).pos;
        for (const Vec3& e : expectMid) {
            if (std::fabs(p.x - e.x) < 1e-5f && std::fabs(p.y - e.y) < 1e-5f &&
                std::fabs(p.z - e.z) < 1e-5f) {
                ++matched;
                break;
            }
        }
    }
    CHECK(matched == 4);

    // Faces adjacent to the ring but outside it (+Y, -Y) keep their shape:
    // the crossed edges are the four VERTICAL edges, so top and bottom remain
    // quads (all 10 faces of the result are quads).
    CHECK(c.m.faceAlive(c.fPlusY));
    CHECK(c.m.faceSideCount(c.fPlusY) == 4);
    CHECK(c.m.faceAlive(c.fMinusY));
    CHECK(c.m.faceSideCount(c.fMinusY) == 4);

    int quads = 0;
    for (const ElemId f : c.m.aliveFaceIds()) {
        if (c.m.faceSideCount(f) == 4) ++quads;
    }
    CHECK(quads == 10);
}

void testLoopCutOpenRingOnCubeWithoutTop() {
    // Open ring on a cube whose top face was deleted: the cut starts on the
    // top-front rim edge (boundary), walks front -> bottom -> back and
    // terminates at the back-top rim (boundary). Every crossed edge has both
    // of its faces inside the ring (or a deleted face), so all resulting
    // faces are quads and the shape is unchanged.
    CubeRef c;
    std::set<ElemId> caps;
    CHECK(ops::deleteElements(c.m, ops::DelKind::Faces, {c.fPlusY}) == ops::Status::OK);
    test::assertInvariants(c.m, "cubo sem topo");
    CHECK(c.m.aliveFaceCount() == 5);

    const ElemId e = test::edgeBetween(c.m, c.v[2], c.v[3]); // top-front rim
    CHECK(e != kNull);
    CHECK(c.m.edgeFaceCount(e) == 1); // boundary edge now
    ops::LoopCutReport rep;
    const ops::Status st = ops::loopCut(c.m, e, rep);
    CHECK(st == ops::Status::OK);
    test::assertInvariants(c.m, "anel aberto no cubo sem topo");
    // V 8->12, E 12->19 (4 splits + 3 loop edges), F 5->8; Euler = 1 (open).
    CHECK(c.m.aliveVertexCount() == 12);
    CHECK(c.m.aliveEdgeCount() == 19);
    CHECK(c.m.aliveFaceCount() == 8);
    CHECK(c.m.aliveVertexCount() - c.m.aliveEdgeCount() + c.m.aliveFaceCount() == 1);
    CHECK(rep.ringFaces.size() == 3);
    CHECK(rep.crossedEdges.size() == 4);
    CHECK(rep.newLoopEdges.size() == 3);
    int quads = 0;
    for (const ElemId f : c.m.aliveFaceIds()) {
        if (c.m.faceSideCount(f) == 4) ++quads;
    }
    CHECK(quads == 8);
}

void testLoopCutOpenRingOnPlane() {
    HalfEdgeMesh m;
    PrimitiveParams p;
    p.cy = 0.0f;
    CHECK(buildPrimitive(PrimType::Plane, p, m));
    const ElemId e = test::edgeBetween(m, 1, 2); // plane boundary edge
    CHECK(e != kNull);

    ops::LoopCutReport rep;
    const ops::Status st = ops::loopCut(m, e, rep);
    CHECK(st == ops::Status::OK);
    test::assertInvariants(m, "loop cut plano (anel aberto)");
    // Open ring: 1 face divided, 2 crossed edges, V 4->6, E 4->7, F 1->2.
    CHECK(rep.ringFaces.size() == 1);
    CHECK(rep.crossedEdges.size() == 2);
    CHECK(rep.newVertices.size() == 2);
    CHECK(rep.newLoopEdges.size() == 1);
    CHECK(m.aliveVertexCount() == 6);
    CHECK(m.aliveEdgeCount() == 7);
    CHECK(m.aliveFaceCount() == 2);
    CHECK(m.aliveVertexCount() - m.aliveEdgeCount() + m.aliveFaceCount() == 1);
    // Both halves are quads.
    for (const ElemId f : m.aliveFaceIds()) {
        CHECK(m.faceSideCount(f) == 4);
    }
}

void testLoopCutRejectsTriangleInRing() {
    // Quad + triangle sharing edge (v2,v3): the ring walk from the quad hits
    // a non-quad face and the cut must be REJECTED with the mesh untouched.
    HalfEdgeMesh m;
    // Quad (1,2,3,4).
    for (const Vec3& p : {Vec3{-1, 0, 1}, Vec3{1, 0, 1}, Vec3{1, 0, -1}, Vec3{-1, 0, -1}}) {
        m.addVertex(p);
    }
    CHECK(m.addFace({1, 2, 3, 4}) != kNull);
    // Tip vertex + triangle on edge (2,3).
    const ElemId tip = m.addVertex({0.0f, 1.0f, 0.0f});
    CHECK(tip != kNull);
    CHECK(m.addFace({3, 2, tip}) != kNull); // (3,2) is the free direction
    m.relinkBoundaryLoops(); // manual build: link boundary hes before validate
    test::assertInvariants(m, "quad+tri antes do loop cut");

    const HalfEdgeMesh::Raw before = m.captureRaw();
    const ElemId e14 = test::edgeBetween(m, 1, 4);
    CHECK(e14 != kNull);

    ops::LoopCutReport rep;
    const ops::Status st = ops::loopCut(m, e14, rep);
    CHECK(st == ops::Status::ErrUnsupported);
    // Mesh byte-identical to the pre-call state.
    CHECK(test::sameMesh(m, test::meshFromRaw(before)));
    test::assertInvariants(m, "quad+tri após rejeição");
}

void testLoopCutRejectsNonQuadStart() {
    // Starting the cut ON the shared quad/triangle edge must also be rejected:
    // one side of the ring is a triangle.
    HalfEdgeMesh m;
    for (const Vec3& p : {Vec3{-1, 0, 1}, Vec3{1, 0, 1}, Vec3{1, 0, -1}, Vec3{-1, 0, -1}}) {
        m.addVertex(p);
    }
    CHECK(m.addFace({1, 2, 3, 4}) != kNull);
    const ElemId tip = m.addVertex({0.0f, 1.0f, 0.0f});
    CHECK(tip != kNull);
    CHECK(m.addFace({3, 2, tip}) != kNull);
    m.relinkBoundaryLoops();
    test::assertInvariants(m, "quad+tri (partida no tri)");

    const ElemId e23 = test::edgeBetween(m, 2, 3); // shared by quad and triangle
    CHECK(e23 != kNull);
    const HalfEdgeMesh::Raw before = m.captureRaw();
    ops::LoopCutReport rep;
    const ops::Status st = ops::loopCut(m, e23, rep);
    CHECK(st == ops::Status::ErrUnsupported);
    CHECK(test::sameMesh(m, test::meshFromRaw(before)));
    test::assertInvariants(m, "quad+tri (partida no tri) após rejeição");
}

} // namespace

namespace blone::test {
void runLoopCutTests() {
    testLoopCutCubeClosedRing();
    testLoopCutOpenRingOnCubeWithoutTop();
    testLoopCutOpenRingOnPlane();
    testLoopCutRejectsTriangleInRing();
    testLoopCutRejectsNonQuadStart();
}
} // namespace blone::test
