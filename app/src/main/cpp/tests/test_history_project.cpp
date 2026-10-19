// History budget (verifiable memory accounting), project round-trips and the
// master acceptance sequence: create cube -> select face -> extrude -> undo ->
// redo -> save -> close -> reopen -> verify.
#include "test_util.h"

#include "../core/history/History.h"
#include "../core/mesh/Primitives.h"
#include "../core/project/Project.h"
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

    // Track a vertex that actually belongs to the selected (top) face.
    const ElemId topVert = *obj.mesh.faceVertices(top).begin();
    const Vec3 before0 = obj.mesh.vrt(topVert).pos;
    CHECK(sceneops::beginTool(s, h, t, sceneops::Tool::Move, dp) == ops::Status::OK);
    CHECK(sceneops::updateTool(s, t, dp, 100.0f, 0.0f) == ops::Status::OK);
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
void runHistoryProjectTests() {
    testHistoryReservationAndBudget();
    testHistoryEviction();
    testTransformToolSession();
    testProjectRoundTrip();
    testProjectRejections();
    testMasterAcceptanceSequence();
}
} // namespace blone::test
