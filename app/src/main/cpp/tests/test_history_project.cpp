// History budget (verifiable memory accounting), project round-trips and the
// master acceptance sequence: create cube -> select face -> extrude -> undo ->
// redo -> save -> close -> reopen -> verify.
#include "test_util.h"

#include "../core/history/History.h"
#include "../core/io/MeshBuilder.h"
#include "../core/io/ObjExporter.h"
#include "../core/io/ObjLoader.h"
#include "../core/mesh/Primitives.h"
#include "../core/project/Project.h"
#include "../core/render/RenderMesh.h"
#include "../core/scene/SceneOps.h"

using namespace blone;

namespace {

void testHistoryReservationAndBudget() {
    Scene s;
    History h;
    const int idx0 = sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0);
    CHECK(idx0 >= 0);
    h.clear();
    h.setBudget(8192); // tiny budget: a cube snapshot bound will not fit
    const int idx = idx0;

    SceneObject& obj = s.objects[static_cast<size_t>(idx)];
    const ElemId top = [&] {
        for (const ElemId f : obj.mesh.aliveFaceIds()) {
            if (obj.mesh.faceNormal(f).y > 0.5f) return f;
        }
        return kNull;
    }();
    CHECK(top != kNull);
    obj.sel.faces.insert(top);

    // Bound = rawBytes*3 must exceed the tiny budget -> REJECTED before
    // mutating: mesh intact, no undo entry.
    const HalfEdgeMesh::Raw before = obj.mesh.captureRaw();
    CHECK(sceneops::extrude(s, h) == ops::Status::ErrBudget);
    CHECK(test::sameMesh(obj.mesh, test::meshFromRaw(before)));
    CHECK(h.undoDepth() == 0);
    CHECK(h.usedBytes() == 0);

    // With a realistic budget the same operation succeeds and the accounted
    // memory is exact (before + after snapshots).
    h.setBudget(History::kDefaultBudget);
    CHECK(sceneops::extrude(s, h) == ops::Status::OK);
    CHECK(h.undoDepth() == 1);
    CHECK(h.usedBytes() == h.lastEntryBytes());
    CHECK(h.usedBytes() > 0);
    // Undo restores the exact pre-state; redo restores the exact post-state.
    const HalfEdgeMesh::Raw post = obj.mesh.captureRaw();
    CHECK(sceneops::undo(s, h) == ops::Status::OK);
    CHECK(test::sameMesh(obj.mesh, test::meshFromRaw(before)));
    CHECK(h.redoDepth() == 1);
    CHECK(sceneops::redo(s, h) == ops::Status::OK);
    CHECK(test::sameMesh(obj.mesh, test::meshFromRaw(post)));

    // A new operation after undo discards the redo branch.
    CHECK(sceneops::undo(s, h) == ops::Status::OK);
    CHECK(h.redoDepth() == 1);
    obj.sel.faces.clear(); // nothing selected; subdivide-all path instead
    CHECK(sceneops::subdivide(s, h) == ops::Status::OK);
    CHECK(h.redoDepth() == 0); // redo branch invalidated
    CHECK(h.undoDepth() >= 1);
}

void testHistoryEviction() {
    Scene s;
    History h;
    // Measure a single extrude entry cost with the default budget first.
    Scene tmp;
    History hTmp;
    CHECK(sceneops::createPrimitive(tmp, hTmp, PrimType::Cube, 0, 0) >= 0);
    hTmp.clear();
    SceneObject& probe = tmp.objects[0];
    probe.sel.faces.insert(probe.mesh.aliveFaceIds()[0]);
    CHECK(sceneops::extrude(tmp, hTmp) == ops::Status::OK);
    const uint64_t entryCost = hTmp.lastEntryBytes();
    CHECK(entryCost > 0);

    h.setBudget(entryCost * 4 + 65536); // fits one bound + margin
    CHECK(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    h.clear();
    int applied = 0;
    for (int i = 0; i < 8; ++i) {
        SceneObject& o = s.objects[0];
        if (o.mesh.aliveFaceCount() == 0) break;
        o.sel.faces.clear();
        o.sel.faces.insert(o.mesh.aliveFaceIds().front());
        const ops::Status st = sceneops::extrude(s, h);
        if (st != ops::Status::OK) break;
        ++applied;
        // Invariant: the budget is never exceeded.
        CHECK(h.usedBytes() <= h.budget());
    }
    CHECK(applied >= 2); // several operations succeeded
    // Eviction happened (depth grew slower than the op count) and at least
    // one entry is retained for undo.
    CHECK(h.undoDepth() >= 1);
    CHECK(h.undoDepth() < applied);
    CHECK(h.undoDepth() <= History::kMaxEntries);
    CHECK(h.usedBytes() <= h.budget());
    // Undo still works after evictions (down to the retained depth).
    int undos = 0;
    while (sceneops::undo(s, h) == ops::Status::OK) ++undos;
    CHECK(undos == static_cast<int>(h.redoDepth()));
}

void testTransformToolSession() {
    Scene s;
    History h;
    CHECK(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    h.clear();
    SceneObject& obj = s.objects[0];
    obj.mesh.vrt(1).pos = Vec3{-1, -1, 1};
    // Select one face and move it with the explicit tool session.
    const ElemId top = [&] {
        for (const ElemId f : obj.mesh.aliveFaceIds()) {
            if (obj.mesh.faceNormal(f).y > 0.5f) return f;
        }
        return kNull;
    }();
    obj.sel.faces.insert(top);
    s.editMode = true;

    sceneops::ToolSession t;
    ops::DragParams dp;
    dp.azimuthDeg = 0.0f;
    dp.elevationDeg = 0.0f;
    dp.distance = 8.0f;
    dp.fovYDeg = 45.0f;
    dp.viewportHeightPx = 1000.0f;

    // Gizmo mapping equivalent to that camera: +X projects to +x_px.
    sceneops::GizmoMapping map;
    map.axisDirX = 1.0f;
    map.axisDirY = 0.0f;
    map.pxPerWorld = 1.0f / ops::worldPerPx(dp);
    map.pivotScreenX = 500.0f;
    map.pivotScreenY = 500.0f;
    map.ringSign = 1.0f;

    // Track a vertex that actually belongs to the selected (top) face.
    const ElemId topVert = *obj.mesh.faceVertices(top).begin();
    const Vec3 before0 = obj.mesh.vrt(topVert).pos;
    CHECK(sceneops::beginTool(s, h, t, sceneops::Tool::Move, 1, 0, 500.0f, 500.0f) ==
          ops::Status::OK);
    CHECK(sceneops::updateTool(s, t, map, 600.0f, 500.0f) == ops::Status::OK);
    CHECK(sceneops::endTool(s, h, t) == ops::Status::OK);
    // Face moved +X by 100 px * worldPerPx; vertices outside untouched.
    const float wpp = ops::worldPerPx(dp);
    CHECK_NEAR(obj.mesh.vrt(topVert).pos.x - before0.x, 100.0f * wpp, 1e-4f);
    CHECK(h.undoDepth() == 1);
    const HalfEdgeMesh::Raw post = obj.mesh.captureRaw();
    CHECK(sceneops::undo(s, h) == ops::Status::OK);
    CHECK_NEAR(obj.mesh.vrt(topVert).pos.x, before0.x, 1e-6f);
    CHECK(sceneops::redo(s, h) == ops::Status::OK);
    CHECK(test::sameMesh(obj.mesh, test::meshFromRaw(post)));
}

void testProjectRoundTrip() {
    Scene s;
    History h;
    CHECK(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    SceneObject& obj = s.objects[0];
    obj.sel.faces.insert(obj.mesh.aliveFaceIds()[0]);
    CHECK(sceneops::extrude(s, h) == ops::Status::OK);
    s.editMode = true;
    s.selectionMode = SelMode::Edge;

    const std::vector<uint8_t> bytes = project::save(s);
    CHECK(!bytes.empty());

    Scene loaded;
    REQUIRE(project::load(bytes, loaded) == project::Ok);
    REQUIRE(loaded.objects.size() == 1);
    CHECK(loaded.objects[0].name == "Cubo");
    CHECK(loaded.editMode);
    CHECK(loaded.selectionMode == SelMode::Edge);
    CHECK(test::sameMesh(loaded.objects[0].mesh, obj.mesh));
    CHECK(loaded.objects[0].sel.faces == obj.sel.faces);
    CHECK(loaded.active == s.active);
}

void testProjectRejections() {
    Scene s;
    History h;
    CHECK(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    const std::vector<uint8_t> good = project::save(s);
    CHECK(good.size() > 24);

    Scene current;
    CHECK(sceneops::createPrimitive(current, h, PrimType::Sphere, 16, 8) >= 0);
    const Scene backup = [&] {
        Scene c;
        const std::vector<uint8_t> b = project::save(current);
        CHECK(project::load(b, c) == project::Ok);
        return c;
    }();

    auto expectReject = [&](std::vector<uint8_t> bad, int err) {
        Scene out = backup; // pretend this is the open project
        CHECK(project::load(bad, out) == err);
        // The current scene must survive untouched.
        CHECK(out.objects.size() == backup.objects.size());
        CHECK(out.objects[0].name == backup.objects[0].name);
        CHECK(test::sameMesh(out.objects[0].mesh, backup.objects[0].mesh));
    };

    // Bad magic.
    std::vector<uint8_t> bad = good;
    bad[0] = 'X';
    expectReject(bad, project::ErrMagic);
    // Newer format version.
    bad = good;
    bad[11] = 99;
    expectReject(bad, project::ErrVersion);
    // Truncated payload.
    bad = good;
    bad.resize(bad.size() - 10);
    expectReject(bad, project::ErrSize);
    // Corrupted payload (CRC mismatch).
    bad = good;
    bad[30] ^= 0xFF;
    expectReject(bad, project::ErrCrc);
    // Declared length mismatch.
    bad = good;
    bad[12] = 0xFF;
    expectReject(bad, project::ErrSize);
}

void testMasterAcceptanceSequence() {
    // The proof sequence from the acceptance criteria, at core level:
    // create cube -> select face -> extrude -> undo -> redo -> save ->
    // new scene -> open -> verify identical.
    Scene s;
    History h;
    const int idx = sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0);
    CHECK(idx == 0);
    SceneObject& obj = s.objects[0];
    const ElemId top = [&] {
        for (const ElemId f : obj.mesh.aliveFaceIds()) {
            if (obj.mesh.faceNormal(f).y > 0.5f) return f;
        }
        return kNull;
    }();
    s.editMode = true; // element picking requires edit mode
    CHECK(sceneops::selectFromPick(s, 3, 0, top) == ops::Status::OK);
    CHECK(obj.sel.faces.count(top) == 1);

    // Extrude.
    CHECK(sceneops::extrude(s, h) == ops::Status::OK);
    CHECK(obj.mesh.aliveFaceCount() == 10);
    CHECK(obj.sel.faces.size() == 1); // selection moved to the cap

    // Undo restores the cube; redo recovers the extrusion.
    CHECK(sceneops::undo(s, h) == ops::Status::OK);
    CHECK(obj.mesh.aliveFaceCount() == 6);
    CHECK(obj.mesh.aliveVertexCount() == 8);
    CHECK(sceneops::redo(s, h) == ops::Status::OK);
    CHECK(obj.mesh.aliveFaceCount() == 10);
    CHECK(obj.mesh.aliveVertexCount() == 12);

    // Save, close, reopen. (Snapshot the post state first: `obj` references
    // the scene vector, which newScene clears.)
    const HalfEdgeMesh::Raw postState = obj.mesh.captureRaw();
    const std::set<ElemId> postSelFaces = obj.sel.faces;
    const std::vector<uint8_t> bytes = project::save(s);
    CHECK(!bytes.empty());
    CHECK(sceneops::newScene(s, h) == ops::Status::OK);
    CHECK(s.objects.empty());
    CHECK(h.undoDepth() == 0);
    Scene reopened;
    REQUIRE(project::load(bytes, reopened) == project::Ok);
    REQUIRE(reopened.objects.size() == 1);
    CHECK(test::sameMesh(reopened.objects[0].mesh, test::meshFromRaw(postState)));
    CHECK(reopened.objects[0].sel.faces == postSelFaces);
    CHECK(reopened.objects[0].mesh.aliveFaceCount() == 10);
}

} // namespace

namespace blone::test {
// ------------------------------------------------ F4-LIMITS P5 ------------
void testProjectV3ReadObjectRoundTrip() {
    using blone::io::MeshBuilder;
    using blone::io::RawMeshData;
    Scene s;
    History h;
    // One EDITABLE object + one READ-ONLY object in the same scene.
    CHECK(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    RawMeshData raw;
    raw.positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0},
                     Vec3{0, 1, 0}};
    raw.colors = {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1},
                  Vec3{1, 1, 0}};
    raw.faceIndices.push_back({0, 1, 2});
    raw.faceIndices.push_back({0, 2, 3});
    auto mesh = MeshBuilder::build(raw);
    REQUIRE(mesh.has_value());
    SceneObject readObj;
    readObj.name = "leitura";
    readObj.position = Vec3{3.0f, 0.0f, 0.0f};
    readObj.editable = false;
    readObj.readMesh =
            std::make_shared<const blone::render::RenderMesh>(
                    blone::render::buildRenderMesh(raw));
    (void)mesh;
    s.objects.push_back(std::move(readObj));
    s.active = 1;
    s.bump();

    // v3 save -> load: the read object survives with its geometry intact.
    const std::vector<uint8_t> bytes = project::save(s);
    REQUIRE(!bytes.empty());
    Scene loaded;
    REQUIRE(project::load(bytes, loaded) == project::Ok);
    REQUIRE(loaded.objects.size() == 2);
    CHECK(loaded.objects[0].editable);
    CHECK(loaded.objects[0].readMesh == nullptr);
    CHECK(test::sameMesh(loaded.objects[0].mesh, s.objects[0].mesh));
    REQUIRE(loaded.objects[1].readMesh != nullptr);
    CHECK(!loaded.objects[1].editable);
    CHECK(loaded.objects[1].mesh.empty());
    CHECK(loaded.objects[1].name == "leitura");
    CHECK(loaded.objects[1].position.x == 3.0f);
    const auto& rm = *loaded.objects[1].readMesh;
    CHECK(rm.positions.size() == 4);
    CHECK(rm.triangleCount == 2);
    CHECK(rm.indices.size() == 6);
    // Bit-exact positions/colors through the serialization.
    for (size_t i = 0; i < raw.positions.size(); ++i) {
        CHECK(rm.positions[i].x == raw.positions[i].x);
        CHECK(rm.positions[i].y == raw.positions[i].y);
        CHECK(rm.positions[i].z == raw.positions[i].z);
        CHECK(rm.colors[i].x == raw.colors[i].x);
        CHECK(rm.colors[i].y == raw.colors[i].y);
        CHECK(rm.colors[i].z == raw.colors[i].z);
    }
    CHECK(loaded.active == 1);
}

void testExportIncludesReadObjects() {
    using blone::io::MeshBuilder;
    using blone::io::ObjExporter;
    using blone::io::ObjLoader;
    using blone::io::RawMeshData;
    Scene s;
    History h;
    CHECK(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    h.clear();
    // A read-only object offset from the editable one.
    RawMeshData raw;
    raw.positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}};
    raw.faceIndices.push_back({0, 1, 2});
    auto readObj = SceneObject{};
    readObj.name = "leitura";
    readObj.position = Vec3{5.0f, 0.0f, 0.0f};
    readObj.editable = false;
    readObj.readMesh =
            std::make_shared<const blone::render::RenderMesh>(
                    blone::render::buildRenderMesh(raw));
    s.objects.push_back(std::move(readObj));
    s.bump();

    // Export: the read-only object's triangle must appear in world space.
    const std::string path = "blone_test_read_export.obj";
    REQUIRE(ObjExporter::exportScene(s, path));
    std::string err;
    const auto back = ObjLoader::load(path, &err);
    REQUIRE(back.has_value());
    // Cube (8 verts) + read triangle expanded (3 corners) — the read
    // triangle is DISJOINT from the cube so it survives dedup.
    bool foundTranslated = false;
    for (const Vec3& p : back->positions) {
        if (p.x > 4.9f && p.x < 5.1f && p.y > -0.1f && p.z > -0.1f) {
            foundTranslated = true; // a vertex of the offset read triangle
        }
    }
    CHECK(foundTranslated);
    // 6 cube quads + 1 read triangle -> 13 triangles after re-import.
    CHECK(blone::io::rawTriangleCount(*back) >= 13);
    std::remove(path.c_str());
}

void runHistoryProjectTests() {
    testHistoryReservationAndBudget();
    testHistoryEviction();
    testTransformToolSession();
    testProjectRoundTrip();
    testProjectRejections();
    testMasterAcceptanceSequence();
    testProjectV3ReadObjectRoundTrip(); // F4-LIMITS P5
    testExportIncludesReadObjects();    // F4-LIMITS P5
}
} // namespace blone::test
