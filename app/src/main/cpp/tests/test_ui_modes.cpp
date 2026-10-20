// Interface reorganization tests (DECISÃO C/D):
//  - OBJECT mode whole-object gizmo transforms (move/rotate/scale) with
//    topology invariants and history round-trips;
//  - mode transitions keep the active object identity and the component
//    selection intact;
//  - duplicateActiveObject: clone counts, offset, activation, undo/redo;
//  - transformDirect step transforms: validation, clamps, history;
//  - the shared gizmo pivot rule (gizmopivot::pivotFor) for both modes.
#include "test_util.h"

#include "../core/scene/GizmoPivot.h"
#include "../core/scene/SceneOps.h"

#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace blone {
namespace {

Scene oneCubeScene(History& h) {
    Scene s;
    const int idx = sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0);
    if (idx < 0) {
        ++g_failures;
        std::printf("FALHOU oneCubeScene: createPrimitive devolveu %d\n", idx);
    }
    h.clear();
    return s;
}

// ---- DECISÃO C: whole-object transforms in OBJECT mode ---------------------

void testObjectModeGizmoTransforms() {
    History h;
    Scene s = oneCubeScene(h);
    REQUIRE(s.activeObject() != nullptr);
    s.editMode = false; // OBJECT mode

    const auto pre = s.objects[0].mesh.captureRaw();
    const auto preCounts = std::make_tuple(s.objects[0].mesh.aliveVertexCount(),
                                           s.objects[0].mesh.aliveEdgeCount(),
                                           s.objects[0].mesh.aliveFaceCount());

    // Move along X via the gizmo session (kind 1 = axis tip).
    sceneops::ToolSession t;
    REQUIRE(sceneops::beginTool(s, h, t, sceneops::Tool::Move, 1, 0, 500.0f, 500.0f) ==
            ops::Status::OK);
    sceneops::GizmoMapping map; // screen +X, 100 px per world unit
    map.axisDirX = 1.0f;
    map.axisDirY = 0.0f;
    map.pxPerWorld = 100.0f;
    REQUIRE(sceneops::updateTool(s, t, map, 650.0f, 500.0f) == ops::Status::OK);
    CHECK_NEAR(t.accumMove.x, 1.5f, 1e-4f); // 150 px / 100 px-per-world
    REQUIRE(sceneops::endTool(s, h, t) == ops::Status::OK);

    // Topology untouched; every vertex moved by +1.5 in X.
    auto& mesh = s.objects[0].mesh;
    CHECK(mesh.aliveVertexCount() == std::get<0>(preCounts));
    CHECK(mesh.aliveEdgeCount() == std::get<1>(preCounts));
    CHECK(mesh.aliveFaceCount() == std::get<2>(preCounts));
    CHECK(mesh.validate());
    for (const auto& v : mesh.vertices()) {
        if (!v.alive) continue;
        CHECK_NEAR(v.pos.x, test::rawPos(pre, v.id).x + 1.5f, 1e-5f);
        CHECK_NEAR(v.pos.y, test::rawPos(pre, v.id).y, 1e-6f);
        CHECK_NEAR(v.pos.z, test::rawPos(pre, v.id).z, 1e-6f);
    }

    // History round-trip.
    const auto moved = mesh.captureRaw();
    REQUIRE(sceneops::undo(s, h) == ops::Status::OK);
    CHECK(test::sameMesh(s.objects[0].mesh, test::meshFromRaw(pre)));
    REQUIRE(sceneops::redo(s, h) == ops::Status::OK);
    CHECK(test::sameMesh(s.objects[0].mesh, test::meshFromRaw(moved)));

    // Rotate the whole object 90° around Z (ring kind 2) — topology intact.
    sceneops::ToolSession r;
    REQUIRE(sceneops::beginTool(s, h, r, sceneops::Tool::Rotate, 2, 2, 600.0f, 500.0f) ==
            ops::Status::OK);
    sceneops::GizmoMapping rmap;
    rmap.pivotScreenX = 500.0f;
    rmap.pivotScreenY = 500.0f;
    rmap.ringSign = 1.0f;
    REQUIRE(sceneops::updateTool(s, r, rmap, 500.0f, 400.0f) == ops::Status::OK); // 90° sweep
    CHECK_NEAR(std::fabs(r.accumDeg), 90.0f, 0.5f);
    REQUIRE(sceneops::endTool(s, h, r) == ops::Status::OK);
    CHECK(s.objects[0].mesh.validate());
    CHECK(s.objects[0].mesh.aliveFaceCount() == std::get<2>(preCounts));

    // Uniform scale x2 via the center handle (kind 3, axis 3).
    sceneops::ToolSession sc;
    REQUIRE(sceneops::beginTool(s, h, sc, sceneops::Tool::Scale, 3, 3, 500.0f, 500.0f) ==
            ops::Status::OK);
    // scaleFromDrag: vertical pixels; not exercised here — apply through two
    // updates to confirm the absolute-from-begin semantics stay consistent.
    REQUIRE(sceneops::updateTool(s, sc, rmap, 500.0f, 350.0f) == ops::Status::OK);
    REQUIRE(sceneops::endTool(s, h, sc) == ops::Status::OK);
    CHECK(s.objects[0].mesh.validate());
    CHECK(s.objects[0].mesh.aliveVertexCount() == std::get<0>(preCounts));
    test::assertInvariants(s.objects[0].mesh, "objeto escalar");
}

// ---- DECISÃO C: mode transitions keep identity + component selection -------

void testModeIdentityAcrossTransitions() {
    History h;
    Scene s = oneCubeScene(h);
    REQUIRE(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0); // second object
    h.clear();
    REQUIRE(s.objects.size() == 2);

    // Activate object 0 (object mode tap => kind 4).
    REQUIRE(sceneops::selectFromPick(s, 4, 0, kNull) == ops::Status::OK);
    CHECK(s.active == 0);

    // Enter EDIT mode, select one face.
    REQUIRE(sceneops::setEditMode(s, true) == ops::Status::OK);
    const ElemId face = s.objects[0].mesh.aliveFaceIds().front();
    REQUIRE(sceneops::selectFromPick(s, 3, 0, face) == ops::Status::OK);
    CHECK(sceneops::hasValidSelection(s));
    int counts[3] = {0, 0, 0};
    sceneops::selectionCounts(s, counts);
    CHECK(counts[2] == 1);

    // Back to OBJECT mode: identity + selection preserved (hidden, not lost).
    REQUIRE(sceneops::setEditMode(s, false) == ops::Status::OK);
    CHECK(s.active == 0);
    CHECK(sceneops::hasValidSelection(s));
    sceneops::selectionCounts(s, counts);
    CHECK(counts[2] == 1);

    // Activate the other object, then return to EDIT: the original object's
    // selection is still there when it becomes active again.
    REQUIRE(sceneops::setActiveObject(s, 1) == ops::Status::OK);
    CHECK(!sceneops::hasValidSelection(s)); // selection belongs to object 0
    REQUIRE(sceneops::setActiveObject(s, 0) == ops::Status::OK);
    REQUIRE(sceneops::setEditMode(s, true) == ops::Status::OK);
    CHECK(sceneops::hasValidSelection(s));
    sceneops::selectionCounts(s, counts);
    CHECK(counts[2] == 1);
    CHECK(s.selectionMode == SelMode::Face);

    // Undo/redo of a transform keeps the selection coherent (DECISÃO C).
    sceneops::ToolSession t;
    REQUIRE(sceneops::beginTool(s, h, t, sceneops::Tool::Move, 1, 0, 0.0f, 0.0f) ==
            ops::Status::OK);
    REQUIRE(sceneops::endTool(s, h, t) == ops::Status::OK); // no movement
    CHECK(sceneops::hasValidSelection(s));
}

// ---- DECISÃO C: duplicate the active object --------------------------------

void testDuplicateActiveObject() {
    History h;
    Scene s = oneCubeScene(h);
    const auto pre = s.objects[0].mesh.captureRaw();

    const int idx = sceneops::duplicateActiveObject(s, h);
    REQUIRE(idx == 1);
    REQUIRE(s.objects.size() == 2);
    CHECK(s.active == 1); // the copy is active
    CHECK(s.objects[1].name == s.objects[0].name);
    CHECK(test::sameMesh(s.objects[1].mesh, test::meshFromRaw(pre)));
    // Visible offset on +X, same Y/Z.
    CHECK(s.objects[1].position.x > s.objects[0].position.x);
    CHECK_NEAR(s.objects[1].position.y, s.objects[0].position.y, 1e-6f);
    // The copy starts with NO selection.
    CHECK(s.objects[1].sel.empty());
    CHECK(!sceneops::hasValidSelection(s));

    // Undo removes the copy and restores the previous active object.
    REQUIRE(sceneops::undo(s, h) == ops::Status::OK);
    CHECK(s.objects.size() == 1);
    CHECK(s.active == 0);
    REQUIRE(sceneops::redo(s, h) == ops::Status::OK);
    CHECK(s.objects.size() == 2);
    CHECK(s.active == 1);
    CHECK(test::sameMesh(s.objects[1].mesh, test::meshFromRaw(pre)));

    // No active object -> error, scene untouched.
    REQUIRE(sceneops::newScene(s, h) == ops::Status::OK);
    h.clear();
    CHECK(sceneops::duplicateActiveObject(s, h) == -static_cast<int>(ops::Status::ErrNoActiveObject));
    CHECK(s.objects.empty());
}

// ---- properties panel: direct step transforms -------------------------------

void testTransformDirectSteps() {
    History h;
    Scene s = oneCubeScene(h);
    auto& mesh = s.objects[0].mesh;
    const auto pre = mesh.captureRaw();

    // Move +0.5 on Y.
    REQUIRE(sceneops::transformDirect(s, h, sceneops::Tool::Move, 1, 0.5f) ==
            ops::Status::OK);
    for (const auto& v : mesh.vertices()) {
        if (!v.alive) continue;
        CHECK_NEAR(v.pos.y, test::rawPos(pre, v.id).y + 0.5f, 1e-6f);
        CHECK_NEAR(v.pos.x, test::rawPos(pre, v.id).x, 1e-6f);
    }
    CHECK(mesh.validate());

    // Rotate 90° around Z (topology intact, pivot = centroid).
    const auto afterMove = mesh.captureRaw();
    REQUIRE(sceneops::transformDirect(s, h, sceneops::Tool::Rotate, 2, 90.0f) ==
            ops::Status::OK);
    CHECK(mesh.validate());
    CHECK(mesh.aliveFaceCount() == 6);
    CHECK(mesh.aliveVertexCount() == 8);
    const auto afterRotate = mesh.captureRaw();

    // Uniform scale x2.
    REQUIRE(sceneops::transformDirect(s, h, sceneops::Tool::Scale, 3, 2.0f) ==
            ops::Status::OK);
    CHECK(mesh.validate());
    test::assertInvariants(mesh, "escala direta");

    // History: three entries, exact reverse.
    REQUIRE(sceneops::undo(s, h) == ops::Status::OK);
    CHECK(test::sameMesh(mesh, test::meshFromRaw(afterRotate)));
    REQUIRE(sceneops::undo(s, h) == ops::Status::OK);
    CHECK(test::sameMesh(mesh, test::meshFromRaw(afterMove)));
    REQUIRE(sceneops::undo(s, h) == ops::Status::OK);
    CHECK(test::sameMesh(mesh, test::meshFromRaw(pre)));

    // Rejections: non-finite, invalid axis, axis 3 for move/rotate.
    CHECK(sceneops::transformDirect(s, h, sceneops::Tool::Move, 0,
                                    std::numeric_limits<float>::quiet_NaN()) ==
          ops::Status::ErrInvalidArg);
    CHECK(sceneops::transformDirect(s, h, sceneops::Tool::Move, 0,
                                    std::numeric_limits<float>::infinity()) ==
          ops::Status::ErrInvalidArg);
    CHECK(sceneops::transformDirect(s, h, sceneops::Tool::Move, 3, 1.0f) ==
          ops::Status::ErrInvalidArg);
    CHECK(sceneops::transformDirect(s, h, sceneops::Tool::Rotate, 3, 15.0f) ==
          ops::Status::ErrInvalidArg);
    CHECK(sceneops::transformDirect(s, h, sceneops::Tool::None, 0, 1.0f) ==
          ops::Status::ErrInvalidArg);
    CHECK(test::sameMesh(mesh, test::meshFromRaw(pre))); // mesh untouched

    // Scale clamps to the same [0.01, 100] window as the gizmo path.
    CHECK(sceneops::transformDirect(s, h, sceneops::Tool::Scale, 3, 1e6f) ==
          ops::Status::OK);
    CHECK(ops::clampAccumScale(1e6f) == ops::kMaxAccumScaleFactor);
    CHECK(mesh.validate());

    // EDIT mode: the step transform targets the VALID selection, not the
    // whole object (documented difference vs OBJECT mode).
    REQUIRE(sceneops::undo(s, h) == ops::Status::OK); // back to un-scaled
    REQUIRE(sceneops::setEditMode(s, true) == ops::Status::OK);
    const ElemId face = mesh.aliveFaceIds().front();
    REQUIRE(sceneops::selectFromPick(s, 3, 0, face) == ops::Status::OK);
    const auto beforeFace = mesh.captureRaw();
    REQUIRE(sceneops::transformDirect(s, h, sceneops::Tool::Move, 1, 0.25f) ==
            ops::Status::OK);
    // Face vertices moved; the opposite face did not.
    bool anyMoved = false;
    bool allMoved = true;
    std::set<ElemId> faceVerts;
    for (const ElemId v : mesh.faceVertices(face)) faceVerts.insert(v);
    for (const auto& v : mesh.vertices()) {
        if (!v.alive) continue;
        const bool moved = std::fabs(v.pos.y - test::rawPos(beforeFace, v.id).y) > 1e-7f;
        if (faceVerts.count(v.id)) {
            anyMoved = anyMoved || moved;
        } else {
            allMoved = allMoved && !moved;
        }
    }
    CHECK(anyMoved);
    CHECK(allMoved);
    CHECK(mesh.validate());
}

// ---- shared pivot rule (renderer + JNI can never diverge) ------------------

void testGizmoPivotRule() {
    History h;
    Scene s = oneCubeScene(h);
    int objIndex = -2;

    // OBJECT mode: centroid of the WHOLE object (cube centered at origin).
    Vec3 pivot{};
    REQUIRE(gizmopivot::pivotFor(s, /*editMode=*/false, pivot, objIndex));
    CHECK(objIndex == 0);
    CHECK_NEAR(pivot.x, 0.0f, 1e-5f);
    CHECK_NEAR(pivot.y, 1.0f, 1e-5f); // primitive cy=1
    CHECK_NEAR(pivot.z, 0.0f, 1e-5f);

    // EDIT mode with a face selected: centroid of THAT face.
    REQUIRE(sceneops::setEditMode(s, true) == ops::Status::OK);
    const ElemId face = s.objects[0].mesh.aliveFaceIds().front();
    REQUIRE(sceneops::selectFromPick(s, 3, 0, face) == ops::Status::OK);
    Vec3 expected{};
    size_t n = 0;
    for (const ElemId v : s.objects[0].mesh.faceVertices(face)) {
        expected = expected + s.objects[0].mesh.vrt(v).pos;
        ++n;
    }
    expected = expected * (1.0f / static_cast<float>(n));
    REQUIRE(gizmopivot::pivotFor(s, true, pivot, objIndex));
    CHECK_NEAR(pivot.x, expected.x, 1e-5f);
    CHECK_NEAR(pivot.y, expected.y, 1e-5f);
    CHECK_NEAR(pivot.z, expected.z, 1e-5f);

    // No active object: no pivot (no gizmo, no hit — no ghost handles).
    REQUIRE(sceneops::newScene(s, h) == ops::Status::OK);
    CHECK(!gizmopivot::pivotFor(s, false, pivot, objIndex));
    CHECK(objIndex == -1);
}

// ---- Scale gizmo handles 21..23: axial scale session (bug fix) -------------
// The renderer returns handle ids 21..23 for the Scale X/Y/Z axis tips (bug:
// they used to be 1..3 like Move, and beginGizmoDrag rejected the drag with
// ErrInvalidArg — "Gizmo: seleção inválida"). beginGizmoDrag maps 21..23 to
// beginTool(Tool::Scale, kind=1, axis=0..2) — THIS session shape is the
// host-testable contract pinned below: axial scale changes ONLY the grabbed
// axis, keeps topology, and round-trips the history.
void testAxialScaleGizmoSession() {
    History h;
    Scene s = oneCubeScene(h);
    auto& mesh = s.objects[0].mesh;
    s.editMode = false; // OBJECT mode: whole active object is the target
    const auto pre = mesh.captureRaw();

    // Handle 21 (Scale X) === Tool::Scale + kind 1 + axis 0.
    sceneops::ToolSession t;
    REQUIRE(sceneops::beginTool(s, h, t, sceneops::Tool::Scale, 1, 0,
                                500.0f, 500.0f) == ops::Status::OK);
    CHECK(t.tool == sceneops::Tool::Scale);
    CHECK(t.kind == 1);
    CHECK(t.axis == 0);

    sceneops::GizmoMapping map; // axis X on screen: +x px, 100 px per world unit
    map.axisDirX = 1.0f;
    map.axisDirY = 0.0f;
    map.pxPerWorld = 100.0f;
    // Drag +150 px along the axis: scaleAxisFromDrag -> factor 2.0.
    REQUIRE(sceneops::updateTool(s, t, map, 650.0f, 500.0f) == ops::Status::OK);
    CHECK_NEAR(t.accumFactor, 2.0f, 1e-5f);
    REQUIRE(sceneops::endTool(s, h, t) == ops::Status::OK);

    // ONLY X doubled around the pivot; Y and Z are untouched; topology intact.
    for (const auto& v : mesh.vertices()) {
        if (!v.alive) continue;
        CHECK_NEAR(v.pos.x, test::rawPos(pre, v.id).x * 2.0f, 1e-5f);
        CHECK_NEAR(v.pos.y, test::rawPos(pre, v.id).y, 1e-6f);
        CHECK_NEAR(v.pos.z, test::rawPos(pre, v.id).z, 1e-6f);
    }
    CHECK(mesh.aliveVertexCount() == 8);
    CHECK(mesh.aliveFaceCount() == 6);
    CHECK(mesh.validate());

    // Undo restores the pre-drag mesh; redo re-applies the axial scale.
    const auto scaled = mesh.captureRaw();
    REQUIRE(sceneops::undo(s, h) == ops::Status::OK);
    CHECK(test::sameMesh(mesh, test::meshFromRaw(pre)));
    REQUIRE(sceneops::redo(s, h) == ops::Status::OK);
    CHECK(test::sameMesh(mesh, test::meshFromRaw(scaled)));

    // Handle 22 (Scale Y) uses axis 1: only Y changes (drag along the Y
    // axis screen direction this time). Axial scale is applied AROUND THE
    // SESSION PIVOT (the vertex centroid, y=1 for the default cube), not the
    // world origin — the expectation below uses that pivot explicitly.
    sceneops::ToolSession ty;
    REQUIRE(sceneops::beginTool(s, h, ty, sceneops::Tool::Scale, 1, 1,
                                500.0f, 500.0f) == ops::Status::OK);
    CHECK(ty.axis == 1);
    CHECK_NEAR(ty.pivot.y, 1.0f, 1e-5f); // centroid of the cube (cy=1)
    // endTool() RESETS the session — capture the pivot while it is alive.
    const float pivotY = ty.pivot.y;
    map.axisDirX = 0.0f;
    map.axisDirY = 1.0f;
    REQUIRE(sceneops::updateTool(s, ty, map, 500.0f, 650.0f) == ops::Status::OK);
    CHECK_NEAR(ty.accumFactor, 2.0f, 1e-5f);
    REQUIRE(sceneops::endTool(s, h, ty) == ops::Status::OK);
    for (const auto& v : mesh.vertices()) {
        if (!v.alive) continue;
        CHECK_NEAR(v.pos.y,
                   pivotY + (test::rawPos(pre, v.id).y - pivotY) * 2.0f,
                   1e-5f);
        CHECK_NEAR(v.pos.x, test::rawPos(pre, v.id).x * 2.0f, 1e-5f);
    }
    CHECK(mesh.validate());

    // A session rejected at begin leaves no active session behind (no stuck
    // transform session, no double session on the next drag).
    sceneops::ToolSession bad;
    CHECK(sceneops::beginTool(s, h, bad, sceneops::Tool::Scale, 4, 0,
                              500.0f, 500.0f) == ops::Status::ErrInvalidArg);
    CHECK(!bad.active);
    sceneops::ToolSession ok;
    REQUIRE(sceneops::beginTool(s, h, ok, sceneops::Tool::Move, 1, 0,
                                500.0f, 500.0f) == ops::Status::OK);
    CHECK(ok.active);
}

} // namespace

namespace test {
void runUiModeTests() {
    testObjectModeGizmoTransforms();
    testModeIdentityAcrossTransitions();
    testDuplicateActiveObject();
    testTransformDirectSteps();
    testGizmoPivotRule();
    testAxialScaleGizmoSession();
}
} // namespace test

} // namespace blone
