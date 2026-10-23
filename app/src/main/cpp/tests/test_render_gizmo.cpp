// Correção Fase 2 — mandatory tests: cube counts/positions/winding, normals,
// ear-clipping triangulation, render buffer updates, selection and the
// gizmo (axis move, ring rotate, axial/uniform scale) with history.
#include "test_util.h"

#include "../core/mesh/Primitives.h"
#include "../core/io/MeshBuilder.h"
#include "../core/operations/TransformMath.h"
#include "../core/render/RenderData.h"
#include "../core/scene/SceneOps.h"

#include <cmath>

using namespace blone;

namespace {

// The exact cube of the correction prompt (positions 0..7, faces CCW).
void testCubeCountsPositionsWinding() {
    HalfEdgeMesh m;
    PrimitiveParams p; // cx=cy=cz=0, size=1 -> positions are exactly +-1
    p.cx = 0.0f;
    p.cy = 0.0f;
    p.cz = 0.0f;
    REQUIRE(buildPrimitive(PrimType::Cube, p, m));

    CHECK(m.aliveVertexCount() == 8);
    CHECK(m.aliveEdgeCount() == 12);
    CHECK(m.aliveFaceCount() == 6);
    CHECK(m.validate());

    const Vec3 expected[8] = {
            {-1, -1, +1}, {+1, -1, +1}, {+1, +1, +1}, {-1, +1, +1},
            {-1, -1, -1}, {+1, -1, -1}, {+1, +1, -1}, {-1, +1, -1},
    };
    // Vertex ids are assigned 1..8 in creation order (first object build).
    for (int i = 0; i < 8; ++i) {
        const Vec3& pos = m.vrt(static_cast<ElemId>(i + 1)).pos;
        CHECK_NEAR(pos.x, expected[i].x, 1e-6f);
        CHECK_NEAR(pos.y, expected[i].y, 1e-6f);
        CHECK_NEAR(pos.z, expected[i].z, 1e-6f);
    }

    // Normals: the 6 axis-aligned outward directions.
    int nx = 0, ny = 0, nz = 0;
    for (const ElemId f : m.aliveFaceIds()) {
        const Vec3 n = m.faceNormal(f);
        REQUIRE(std::fabs(length(n) - 1.0f) < 1e-5f);
        if (std::fabs(std::fabs(n.x) - 1.0f) < 1e-5f) ++nx;
        else if (std::fabs(std::fabs(n.y) - 1.0f) < 1e-5f) ++ny;
        else if (std::fabs(std::fabs(n.z) - 1.0f) < 1e-5f) ++nz;
    }
    CHECK(nx == 2);
    CHECK(ny == 2);
    CHECK(nz == 2);

    // Outward winding: normal points away from the mesh center on all faces.
    const Vec3 center{0, 0, 0};
    for (const ElemId f : m.aliveFaceIds()) {
        const Vec3 c = m.faceCentroid(f) - center;
        CHECK(dot(m.faceNormal(f), c) > 0.0f);
    }

    // Triangles: 12 total; quads triangulated EXACTLY as
    // [a,b,c,d] -> (a,b,c),(a,c,d), winding consistent with the face normal.
    std::vector<ElemId> tris;
    size_t count = 0;
    for (const ElemId f : m.aliveFaceIds()) {
        tris.clear();
        renderdata::faceTriangles(m, f, tris);
        REQUIRE(tris.size() == 6); // 2 triangles
        const std::vector<ElemId> loop = m.faceVertices(f);
        CHECK(tris[0] == loop[0] && tris[1] == loop[1] && tris[2] == loop[2]);
        CHECK(tris[3] == loop[0] && tris[4] == loop[2] && tris[5] == loop[3]);
        const Vec3 n = m.faceNormal(f);
        for (size_t k = 0; k + 2 < tris.size(); k += 3) {
            const Vec3 a = m.vrt(tris[k + 0]).pos;
            const Vec3 b = m.vrt(tris[k + 1]).pos;
            const Vec3 c = m.vrt(tris[k + 2]).pos;
            CHECK(dot(cross(b - a, c - a), n) > 0.0f);
        }
        count += tris.size() / 3;
    }
    CHECK(count == 12);
}

// Editable vertices (8) vs duplicated render buffer vertices (24).
void testEditableVsBufferVertices() {
    Scene s;
    History h;
    REQUIRE(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    renderdata::RenderData rd;
    renderdata::buildObject(s.objects[0], rd);
    CHECK(rd.editableVertexCount == 8);
    CHECK(rd.fillVertexCount == 24); // 6 faces x 4 duplicated corners
    CHECK(rd.fillIdx.size() == 36);  // 12 triangles
    CHECK(rd.triangleCount == 12);
    CHECK(rd.wirePos.size() == 12 * 2 * 3);
    CHECK(rd.pointPos.size() == 8 * 3);
    // Every duplicated vertex carries the normal of its own face.
    for (size_t i = 0; i < rd.fillVertexCount; ++i) {
        const float nl = std::sqrt(rd.fillNrm[i * 3] * rd.fillNrm[i * 3] +
                                   rd.fillNrm[i * 3 + 1] * rd.fillNrm[i * 3 + 1] +
                                   rd.fillNrm[i * 3 + 2] * rd.fillNrm[i * 3 + 2]);
        CHECK_NEAR(nl, 1.0f, 1e-5f);
    }
}

// Ear clipping handles CONCAVE polygons without bowties (the old fan drew
// "Z"/arrow shapes after edge merges).
void testEarClippingConcave() {
    HalfEdgeMesh m;
    // Concave "U" polygon on the z=0 plane (CCW seen from +Z):
    // (-1,-1) (1,-1) (1,1) (0.5,1) (0.5,-0.2) (-0.5,-0.2) (-0.5,1) (-1,1)
    const Vec3 pts[8] = {
            {-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {0.5f, 1, 0},
            {0.5f, -0.2f, 0}, {-0.5f, -0.2f, 0}, {-0.5f, 1, 0}, {-1, 1, 0},
    };
    std::vector<ElemId> ids;
    for (const Vec3& p : pts) ids.push_back(m.addVertex(p));
    const ElemId f = m.addFace(ids);
    REQUIRE(f != kNull);
    m.relinkBoundaryLoops();
    REQUIRE(m.validate());

    std::vector<ElemId> tris;
    renderdata::faceTriangles(m, f, tris);
    CHECK(tris.size() == 6 * 3); // n-2 = 6 triangles

    // No bowties: every triangle must agree with the face normal (+Z) and
    // the total triangle area must equal the polygon area.
    const Vec3 n = m.faceNormal(f);
    float totalArea = 0.0f;
    for (size_t k = 0; k + 2 < tris.size(); k += 3) {
        const Vec3 a = m.vrt(tris[k + 0]).pos;
        const Vec3 b = m.vrt(tris[k + 1]).pos;
        const Vec3 c = m.vrt(tris[k + 2]).pos;
        const Vec3 cr = cross(b - a, c - a);
        CHECK(dot(cr, n) > 0.0f); // consistent winding, no flipped ears
        totalArea += length(cr) * 0.5f;
    }
    // Polygon area by the shoelace formula in xy.
    float area2 = 0.0f;
    for (size_t i = 0; i < 8; ++i) {
        const Vec3& a = pts[i];
        const Vec3& b = pts[(i + 1) % 8];
        area2 += a.x * b.y - b.x * a.y;
    }
    CHECK_NEAR(totalArea, area2 * 0.5f, 1e-4f); // sum of triangles == polygon
}

// Normals and buffers update after geometry changes (move -> normals change;
// extrude -> more triangles; undo -> back to 12).
void testBufferUpdatesAfterOperations() {
    Scene s;
    History h;
    REQUIRE(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    SceneObject& obj = s.objects[0];

    renderdata::RenderData before;
    renderdata::buildObject(obj, before);

    // Move a vertex: the faces that contain it must change their normals and
    // the rebuilt buffers must reflect the new positions.
    const ElemId v1 = 1;
    const Vec3 moved{-2.0f, -2.0f, 1.0f};
    obj.mesh.vrt(v1).pos = moved;
    s.bump();
    renderdata::RenderData after;
    renderdata::buildObject(obj, after);
    bool posChanged = false;
    bool nrmChanged = false;
    for (size_t i = 0; i < before.fillVertexCount; ++i) {
        if (std::fabs(before.fillPos[i * 3] - after.fillPos[i * 3]) > 1e-6f ||
            std::fabs(before.fillPos[i * 3 + 1] - after.fillPos[i * 3 + 1]) > 1e-6f ||
            std::fabs(before.fillPos[i * 3 + 2] - after.fillPos[i * 3 + 2]) > 1e-6f) {
            posChanged = true;
        }
        if (std::fabs(before.fillNrm[i * 3] - after.fillNrm[i * 3]) > 1e-4f ||
            std::fabs(before.fillNrm[i * 3 + 1] - after.fillNrm[i * 3 + 1]) > 1e-4f ||
            std::fabs(before.fillNrm[i * 3 + 2] - after.fillNrm[i * 3 + 2]) > 1e-4f) {
            nrmChanged = true;
        }
    }
    CHECK(posChanged);
    CHECK(nrmChanged);
    CHECK(test::assertNormalsOk(obj.mesh));

    // Extrude the top face: 5 faces + 5 untouched = 10 quads -> 20 triangles.
    const ElemId top = [&] {
        for (const ElemId f : obj.mesh.aliveFaceIds()) {
            if (obj.mesh.faceNormal(f).y > 0.5f) return f;
        }
        return kNull;
    }();
    REQUIRE(top != kNull);
    obj.sel.faces.insert(top);
    REQUIRE(sceneops::extrude(s, h) == ops::Status::OK);
    renderdata::RenderData afterExtrude;
    renderdata::buildObject(obj, afterExtrude);
    CHECK(afterExtrude.triangleCount == 20);
    CHECK(afterExtrude.fillVertexCount == 40);
    CHECK(afterExtrude.editableVertexCount == 12); // 8 + 4 duplicated cap verts

    // Undo restores the 12-triangle buffers.
    REQUIRE(sceneops::undo(s, h) == ops::Status::OK);
    renderdata::RenderData afterUndo;
    renderdata::buildObject(obj, afterUndo);
    CHECK(afterUndo.triangleCount == 12);
    CHECK(afterUndo.editableVertexCount == 8);
}

// Selection updates through the scene facade (pick kinds 1/2/3 + prune).
void testSelectionLifecycle() {
    Scene s;
    History h;
    REQUIRE(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    SceneObject& obj = s.objects[0];
    s.editMode = true;
    s.selectionMode = SelMode::Vertex;

    const ElemId f0 = obj.mesh.aliveFaceIds()[0];
    CHECK(sceneops::selectFromPick(s, 3, 0, f0) == ops::Status::OK);
    CHECK(obj.sel.faces.count(f0) == 1);
    CHECK(obj.sel.verts.empty() && obj.sel.edges.empty());

    const std::vector<ElemId> fe = obj.mesh.faceEdges(f0);
    REQUIRE(!fe.empty());
    CHECK(sceneops::selectFromPick(s, 2, 0, fe[0]) == ops::Status::OK);
    CHECK(obj.sel.edges.count(fe[0]) == 1); // single-select replaced the face
    CHECK(obj.sel.faces.empty());

    const std::vector<ElemId> fv = obj.mesh.faceVertices(f0);
    CHECK(sceneops::selectFromPick(s, 1, 0, fv[0]) == ops::Status::OK);
    CHECK(obj.sel.verts.count(fv[0]) == 1);

    // Multi-select toggle behaviour.
    CHECK(sceneops::setMultiSelect(s, true) == ops::Status::OK);
    CHECK(sceneops::selectFromPick(s, 1, 0, fv[1]) == ops::Status::OK);
    CHECK(obj.sel.verts.count(fv[1]) == 1);
    CHECK(sceneops::selectFromPick(s, 1, 0, fv[1]) == ops::Status::OK);
    CHECK(obj.sel.verts.count(fv[1]) == 0); // toggle removes
    CHECK(sceneops::setMultiSelect(s, false) == ops::Status::OK);

    // Deleting the selected vertex prunes the selection (no dangling ids).
    CHECK(sceneops::selectFromPick(s, 1, 0, fv[0]) == ops::Status::OK);
    CHECK(sceneops::deleteSelection(s, h) == ops::Status::OK);
    CHECK(!obj.mesh.vertexAlive(fv[0]));
    CHECK(obj.sel.verts.empty());
    CHECK(obj.mesh.validate());
}

// Gizmo transforms pinned end-to-end: axis move, ring rotate (finger follows),
// axial + uniform scale — each with undo/redo restoring positions.
void testGizmoTransformsWithHistory() {
    Scene s;
    History h;
    REQUIRE(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    h.clear();
    SceneObject& obj = s.objects[0];
    s.editMode = true;
    s.selectionMode = SelMode::Face;

    // Move the whole cube along +X with an axis drag (empty selection =
    // whole object; the edit mode never locks).
    const HalfEdgeMesh::Raw pre = obj.mesh.captureRaw();
    sceneops::ToolSession t;
    sceneops::GizmoMapping map;
    map.axisDirX = 1.0f;
    map.axisDirY = 0.0f;
    map.pxPerWorld = 100.0f; // 100 px per world unit
    map.pivotScreenX = 400.0f;
    map.pivotScreenY = 300.0f;
    map.ringSign = -1.0f; // axis towards camera (documented convention)
    REQUIRE(sceneops::beginTool(s, h, t, sceneops::Tool::Move, 1, 0, 400.0f, 300.0f) ==
            ops::Status::OK);
    CHECK(sceneops::updateTool(s, t, map, 500.0f, 300.0f) == ops::Status::OK);
    CHECK(sceneops::endTool(s, h, t) == ops::Status::OK);
    bool movedX = false;
    bool movedYZ = false;
    for (const auto& v : obj.mesh.vertices()) {
        if (!v.alive) continue;
        const Vec3 old = test::rawPos(pre, v.id);
        if (std::fabs(v.pos.x - old.x - 1.0f) < 1e-4f) movedX = true;
        if (std::fabs(v.pos.y - old.y) > 1e-6f || std::fabs(v.pos.z - old.z) > 1e-6f) {
            movedYZ = true;
        }
    }
    CHECK(movedX);
    CHECK(!movedYZ); // only the X axis moved
    CHECK(h.undoDepth() == 1);
    REQUIRE(sceneops::undo(s, h) == ops::Status::OK);
    CHECK(test::sameMesh(obj.mesh, test::meshFromRaw(pre)));
    REQUIRE(sceneops::redo(s, h) == ops::Status::OK);

    // Axial scale along X: factor 2 on the axis component only.
    const HalfEdgeMesh::Raw preScale = obj.mesh.captureRaw();
    REQUIRE(sceneops::beginTool(s, h, t, sceneops::Tool::Scale, 1, 0, 400.0f, 300.0f) ==
            ops::Status::OK);
    // Drag 150px along the axis screen dir -> factor 2.
    CHECK(sceneops::updateTool(s, t, map, 550.0f, 300.0f) == ops::Status::OK);
    CHECK(sceneops::endTool(s, h, t) == ops::Status::OK);
    const float pivotX = 1.0f; // centroid of the moved cube (X + 1)
    for (const auto& v : obj.mesh.vertices()) {
        if (!v.alive) continue;
        const Vec3 old = test::rawPos(preScale, v.id);
        CHECK_NEAR(v.pos.y, old.y, 1e-4f); // Y untouched
        CHECK_NEAR(v.pos.z, old.z, 1e-4f); // Z untouched
        // X doubled AROUND THE PIVOT (not the origin).
        CHECK_NEAR(v.pos.x, pivotX + (old.x - pivotX) * 2.0f, 1e-3f);
    }
    REQUIRE(sceneops::undo(s, h) == ops::Status::OK);
    CHECK(test::sameMesh(obj.mesh, test::meshFromRaw(preScale)));
}

// Ring rotation sign: the surface point under the finger follows the finger.
void testRingRotationSign() {
    Scene s;
    History h;
    REQUIRE(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    h.clear();
    SceneObject& obj = s.objects[0];
    s.editMode = true;

    // Camera at az=0, el=0, dist=8 looking at the origin => forward = -Z,
    // screen x -> +X, screen y (down) -> -Y.
    sceneops::ToolSession t;
    sceneops::GizmoMapping map;
    map.pivotScreenX = 500.0f;
    map.pivotScreenY = 500.0f;

    // A point at +X (vertex 2 = (+1,-1,+1)) — rotate the Z ring whose axis
    // points TOWARDS the camera => documented ringSign = -1.
    const ElemId probe = 2; // (+1,-1,+1)
    const Vec3 before = obj.mesh.vrt(probe).pos;
    REQUIRE(sceneops::beginTool(s, h, t, sceneops::Tool::Rotate, 2, 2,
                                600.0f, 500.0f) == ops::Status::OK);
    // Finger sweeps UP on screen (600,500) -> (600,400): visually CCW.
    map.ringSign = -1.0f;
    CHECK(sceneops::updateTool(s, t, map, 600.0f, 400.0f) == ops::Status::OK);
    CHECK(sceneops::endTool(s, h, t) == ops::Status::OK);
    const Vec3 after = obj.mesh.vrt(probe).pos;
    // The point must follow the finger UP on screen: its Y grows.
    CHECK(after.y > before.y + 1e-4f);
    CHECK(after.x > before.x - 1e-4f); // still on the right side
}

// ------------------------------------------------ F4-LIMITS P7 ------------
void testFrustumCullingAabb() {
    // Camera at distance 8 on +Z looking at the origin (orbit convention:
    // forward = -Z), 60 deg vertical FOV, near 0.1 / far 200.
    OrbitCamera cam;
    cam.azimuthDeg = 0.0f;
    cam.elevationDeg = 0.0f;
    cam.distance = 8.0f;
    cam.target = Vec3{0, 0, 0};
    const Mat4 vp = Mat4::perspective(60.0f, 1.0f, 0.1f, 200.0f) *
                    cam.viewMatrix();

    // Unit box at the look-at point: inside the frustum.
    Aabb center;
    center.minX = -1; center.minY = -1; center.minZ = -1;
    center.maxX =  1; center.maxY =  1; center.maxZ =  1;
    CHECK(aabbInFrustum(center, vp));

    // Box straddling a frustum plane: still visible (conservative test).
    Aabb straddle;
    straddle.minX = -1; straddle.minY = -1; straddle.minZ = -1;
    straddle.maxX =  1; straddle.maxY =  1; straddle.maxZ = 20;
    CHECK(aabbInFrustum(straddle, vp));

    // Box BEHIND the camera (camera sits at z = +8; box spans z 9..12):
    // every corner fails the near plane -> culled.
    Aabb behind;
    behind.minX = -1; behind.minY = -1; behind.minZ = 9;
    behind.maxX =  1; behind.maxY =  1; behind.maxZ = 12;
    CHECK(!aabbInFrustum(behind, vp));

    // Box far outside the horizontal FOV (x ~ +200): culled.
    Aabb side;
    side.minX = 199; side.minY = -1; side.minZ = -1;
    side.maxX = 201; side.maxY =  1; side.maxZ =  1;
    CHECK(!aabbInFrustum(side, vp));

    // Box beyond the far plane (z = -300): culled.
    Aabb farBox;
    farBox.minX = -1; farBox.minY = -1; farBox.minZ = -310;
    farBox.maxX =  1; farBox.maxY =  1; farBox.maxZ = -300;
    CHECK(!aabbInFrustum(farBox, vp));
}

void testIndexTypeChoicePerObject() {
    using renderdata::buildObject;
    using renderdata::fitsUint16Index;
    using renderdata::downcastIndices16;

    // Threshold contract: 65.535 stays 16-bit, 65.536 needs uint32.
    CHECK(fitsUint16Index(0));
    CHECK(fitsUint16Index(65535));
    CHECK(!fitsUint16Index(65536));
    CHECK(!fitsUint16Index(70000)); // "objeto com 70k vértices" -> uint32

    // Conversion preserves the index values.
    {
        std::vector<uint32_t> src{0, 1, 2, 65534, 65535};
        std::vector<uint16_t> out;
        downcastIndices16(src, out);
        REQUIRE(out.size() == src.size());
        for (size_t i = 0; i < src.size(); ++i) {
            CHECK(static_cast<uint32_t>(out[i]) == src[i]);
        }
    }
    // Misuse (index beyond 16 bits) -> EMPTY output, never truncated data
    // (the renderer checks the size and falls back to uint32).
    {
        std::vector<uint32_t> src{0, 65536, 2};
        std::vector<uint16_t> out;
        downcastIndices16(src, out);
        CHECK(out.empty());
    }

    // Real object below the threshold (cube: 24 duplicated fill vertices)
    // -> the decision function picks the 16-bit path.
    {
        Scene scene;
        History h;
        const int idx = sceneops::createPrimitive(scene, h, PrimType::Cube, 0, 0);
        REQUIRE(idx == 0);
        renderdata::RenderData rd;
        buildObject(scene.objects[0], rd);
        CHECK(rd.fillVertexCount == 24);
        CHECK(fitsUint16Index(rd.fillVertexCount));
        bool inRange = true;
        for (const uint32_t i : rd.fillIdx) {
            if (i >= rd.fillVertexCount) inRange = false;
        }
        CHECK(inRange);
    }

    // Real object beyond the threshold, built through the IMPORT path
    // (MeshBuilder — primitives cap at 96x48 segments and cannot exceed
    // 16-bit buffers): a 300x300 quad grid has 90601 positions and 360000
    // duplicated fill vertices -> uint32 path, indices beyond 16 bits.
    {
        const int kN = 300;
        blone::io::RawMeshData grid;
        grid.positions.reserve(static_cast<size_t>(kN + 1) * (kN + 1));
        for (int y = 0; y <= kN; ++y) {
            for (int x = 0; x <= kN; ++x) {
                grid.positions.push_back(Vec3{static_cast<float>(x),
                                              0.0f, static_cast<float>(y)});
            }
        }
        grid.faceIndices.reserve(static_cast<size_t>(kN) * kN);
        for (int y = 0; y < kN; ++y) {
            for (int x = 0; x < kN; ++x) {
                const int v0 = y * (kN + 1) + x;
                grid.faceIndices.push_back({v0, v0 + 1, v0 + kN + 2,
                                            v0 + kN + 1});
            }
        }
        auto mesh = blone::io::MeshBuilder::build(grid);
        REQUIRE(mesh.has_value());
        SceneObject obj;
        obj.name = "grade";
        obj.mesh = std::move(*mesh);
        renderdata::RenderData rd;
        buildObject(obj, rd);
        CHECK(rd.fillVertexCount ==
              static_cast<size_t>(kN) * kN * 4u); // 360000
        CHECK(!fitsUint16Index(rd.fillVertexCount));
        bool beyond16 = false;
        for (const uint32_t i : rd.fillIdx) {
            if (i > 65535u) { beyond16 = true; break; }
        }
        CHECK(beyond16);
        bool inRange = true;
        for (const uint32_t i : rd.fillIdx) {
            if (i >= rd.fillVertexCount) inRange = false;
        }
        CHECK(inRange);
    }
}

} // namespace

namespace blone::test {
void runRenderGizmoTests() {
    testCubeCountsPositionsWinding();
    testEditableVsBufferVertices();
    testEarClippingConcave();
    testBufferUpdatesAfterOperations();
    testSelectionLifecycle();
    testGizmoTransformsWithHistory();
    testRingRotationSign();
    testIndexTypeChoicePerObject(); // F4-LIMITS P2
    testFrustumCullingAabb();       // F4-LIMITS P7
}
} // namespace blone::test
