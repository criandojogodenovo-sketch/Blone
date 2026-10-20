// Correção de estabilidade — mandatory tests: selection single source of
// truth (atomic counts, empty/valid/invalid states), selection validity
// after every operation (extrude/subdivide/delete/loop cut/undo/redo),
// scale rejection (zero / non-finite / accumulation clamps) and extrusion
// winding + normals with culling-safe orientation.
#include "test_util.h"

#include "../core/mesh/Primitives.h"
#include "../core/operations/TransformMath.h"
#include "../core/render/RenderData.h"
#include "../core/scene/SceneOps.h"

#include <cmath>
#include <limits>

using namespace blone;

namespace {

// Top face of a default cube (normal +Y).
ElemId topFaceOf(const HalfEdgeMesh& m) {
    for (const ElemId f : m.aliveFaceIds()) {
        if (m.faceNormal(f).y > 0.5f) return f;
    }
    return kNull;
}

// First alive edge id (there is no aliveEdgeIds() on the mesh by design).
ElemId firstAliveEdge(const HalfEdgeMesh& m) {
    for (const auto& e : m.edges()) {
        if (e.alive) return e.id;
    }
    return kNull;
}

// ------------------------------------------------------- selection state ----

// Empty selection: no overlay geometry, no valid selection, zero counts.
void testEmptySelectionProducesNoOverlay() {
    Scene s;
    History h;
    REQUIRE(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    h.clear();
    s.editMode = true;

    CHECK(!sceneops::hasValidSelection(s));
    int counts[3] = {-1, -1, -1};
    sceneops::selectionCounts(s, counts);
    CHECK(counts[0] == 0 && counts[1] == 0 && counts[2] == 0);

    SceneObject& obj = s.objects[0];
    renderdata::SelectionData sd;
    renderdata::buildSelection(obj, sd);
    CHECK(sd.facePos.empty() && sd.faceIdx.empty());
    CHECK(sd.edgePos.empty());
    CHECK(sd.vertPos.empty());

    // Clearing an already-empty selection keeps the state consistent.
    CHECK(sceneops::clearSelection(s) == ops::Status::OK);
    sceneops::selectionCounts(s, counts);
    CHECK(counts[0] == 0 && counts[1] == 0 && counts[2] == 0);
    CHECK(!sceneops::hasValidSelection(s));
}

// Stale ids are not counted, are pruned by pruneAllSelections and never
// produce overlay geometry (empty/invalid never looks like a render bug).
void testStaleIdsAreRejectedEverywhere() {
    Scene s;
    History h;
    REQUIRE(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    h.clear();
    SceneObject& obj = s.objects[0];
    s.editMode = true;

    const ElemId f0 = obj.mesh.aliveFaceIds()[0];
    REQUIRE(sceneops::selectFromPick(s, 3, 0, f0) == ops::Status::OK);
    CHECK(sceneops::hasValidSelection(s));

    // Selecting a DEAD id must be refused (nothing enters the selection).
    const ElemId dead = 999999;
    CHECK(sceneops::selectFromPick(s, 3, 0, dead) == ops::Status::ErrInvalidArg);
    CHECK(obj.sel.faces.size() == 1);

    // Injecting a stale id (simulating a bug): counts ignore it, overlays
    // skip it and the defensive prune removes it.
    obj.sel.faces.insert(dead);
    int counts[3] = {0, 0, 0};
    sceneops::selectionCounts(s, counts);
    CHECK(counts[2] == 1); // only the alive face counts

    renderdata::SelectionData sd;
    renderdata::buildSelection(obj, sd);
    CHECK(!sd.facePos.empty());    // the alive face still renders its tint
    CHECK(sd.faceIdx.size() == 6); // one quad -> 2 triangles (dead one skipped)

    CHECK(sceneops::pruneAllSelections(s));
    CHECK(obj.sel.faces.size() == 1);
    CHECK(!obj.sel.faces.count(dead));
    CHECK(sceneops::hasValidSelection(s));

    // Clearing removes everything: no gizmo, no markers, no highlights.
    CHECK(sceneops::clearSelection(s) == ops::Status::OK);
    renderdata::buildSelection(obj, sd);
    CHECK(sd.facePos.empty() && sd.edgePos.empty() && sd.vertPos.empty());
    CHECK(!sceneops::hasValidSelection(s));
}

// Every topology operation leaves an alive-only selection.
void testSelectionValidAfterOperations() {
    Scene s;
    History h;
    REQUIRE(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    h.clear();
    SceneObject& obj = s.objects[0];
    s.editMode = true;
    s.selectionMode = SelMode::Face;
    int counts[3] = {0, 0, 0};

    // Extrude: selection moves to the new cap faces (all alive).
    const ElemId top = topFaceOf(obj.mesh);
    REQUIRE(top != kNull);
    REQUIRE(sceneops::selectFromPick(s, 3, 0, top) == ops::Status::OK);
    REQUIRE(sceneops::extrude(s, h) == ops::Status::OK);
    for (const ElemId f : obj.sel.faces) CHECK(obj.mesh.faceAlive(f));
    CHECK(obj.sel.verts.empty() && obj.sel.edges.empty());
    sceneops::selectionCounts(s, counts);
    CHECK(counts[2] == static_cast<int>(obj.sel.faces.size()));
    CHECK(counts[2] > 0);

    // Undo of the extrude: History restores the PRE-op selection (the top
    // face, alive again) — a valid, alive-only selection state.
    REQUIRE(sceneops::undo(s, h) == ops::Status::OK);
    sceneops::selectionCounts(s, counts);
    CHECK(counts[2] == 1);
    CHECK(counts[0] == 0 && counts[1] == 0);
    CHECK(sceneops::hasValidSelection(s));
    for (const ElemId f : obj.sel.faces) CHECK(obj.mesh.faceAlive(f));

    // Redo restores the caps and the selection moves with them.
    REQUIRE(sceneops::redo(s, h) == ops::Status::OK);
    for (const ElemId f : obj.sel.faces) CHECK(obj.mesh.faceAlive(f));
    CHECK(!obj.sel.faces.empty());

    // Loop cut on the all-quad extruded cube clears the selection; nothing
    // dangling remains. (A loop cut AFTER the subdivision below would be
    // correctly REJECTED with ErrUnsupported: subdivision leaves 6-sided
    // neighbour faces — documented behaviour — and the ring walk refuses
    // non-quad rings. The operation is refused whole, never partial.)
    REQUIRE(sceneops::setSelectionMode(s, SelMode::Edge) == ops::Status::OK);
    const ElemId e0 = firstAliveEdge(obj.mesh);
    REQUIRE(e0 != kNull);
    REQUIRE(sceneops::selectFromPick(s, 2, 0, e0) == ops::Status::OK);
    REQUIRE(sceneops::loopCut(s, h) == ops::Status::OK);
    CHECK(obj.sel.empty());
    CHECK(obj.mesh.validate());

    // Subdivide (whole mesh when nothing is selected) clears the selection;
    // the mesh stays valid.
    REQUIRE(sceneops::setSelectionMode(s, SelMode::Face) == ops::Status::OK);
    REQUIRE(sceneops::subdivide(s, h) == ops::Status::OK);
    CHECK(obj.sel.empty());
    CHECK(obj.mesh.validate());

    // Delete selection prunes to empty (nothing survives its own deletion).
    REQUIRE(sceneops::setSelectionMode(s, SelMode::Face) == ops::Status::OK);
    const ElemId someFace = obj.mesh.aliveFaceIds()[0];
    REQUIRE(sceneops::selectFromPick(s, 3, 0, someFace) == ops::Status::OK);
    REQUIRE(sceneops::deleteSelection(s, h) == ops::Status::OK);
    sceneops::selectionCounts(s, counts);
    CHECK(counts[0] == 0 && counts[1] == 0 && counts[2] == 0);
    CHECK(obj.mesh.validate());
    CHECK(!sceneops::hasValidSelection(s));
}

// ------------------------------------------------------- scale safety -------

// Non-finite touch input and degenerate factors are rejected with the mesh
// untouched; accumulated scale stays within [0.01, 100].
void testScaleRejectionAndClamps() {
    Scene s;
    History h;
    REQUIRE(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    h.clear();
    SceneObject& obj = s.objects[0];
    s.editMode = true;
    s.selectionMode = SelMode::Face;

    const ElemId top = topFaceOf(obj.mesh);
    REQUIRE(top != kNull);
    REQUIRE(sceneops::selectFromPick(s, 3, 0, top) == ops::Status::OK);
    const HalfEdgeMesh::Raw pre = obj.mesh.captureRaw();

    sceneops::ToolSession t;
    sceneops::GizmoMapping map;
    map.axisDirX = 1.0f;
    map.axisDirY = 0.0f;
    map.pxPerWorld = 100.0f;
    map.pivotScreenX = 400.0f;
    map.pivotScreenY = 300.0f;
    map.ringSign = 1.0f;

    // Uniform scale (center handle): NaN touch is rejected, mesh untouched.
    REQUIRE(sceneops::beginTool(s, h, t, sceneops::Tool::Scale, 3, 3, 400.0f, 300.0f) ==
            ops::Status::OK);
    CHECK(sceneops::updateTool(s, t, map,
                               std::numeric_limits<float>::quiet_NaN(), 300.0f) ==
          ops::Status::ErrInvalidArg);
    CHECK(test::sameMesh(obj.mesh, test::meshFromRaw(pre)));
    sceneops::cancelTool(s, h, t);
    CHECK(test::sameMesh(obj.mesh, test::meshFromRaw(pre)));

    // Axial scale: accumulation is clamped into [0.01, 100] — a long shrink
    // drag can never flatten the mesh to zero (min positive 0.01).
    REQUIRE(sceneops::beginTool(s, h, t, sceneops::Tool::Scale, 1, 0, 400.0f, 300.0f) ==
            ops::Status::OK);
    bool allFinite = true;
    for (int i = 0; i < 200; ++i) {
        // Each step multiplies by ~0.02 (per-event clamp floor).
        CHECK(sceneops::updateTool(s, t, map, 400.0f - 300.0f * (i + 1), 300.0f) ==
              ops::Status::OK);
    }
    for (const auto& v : obj.mesh.vertices()) {
        if (!v.alive) continue;
        if (!ops::isFiniteVec(v.pos.x, v.pos.y, v.pos.z)) allFinite = false;
    }
    CHECK(allFinite);
    // The X extent shrank but never collapsed to exactly zero.
    float minX = 1e9f, maxX = -1e9f;
    for (const auto& v : obj.mesh.vertices()) {
        if (!v.alive) continue;
        minX = std::min(minX, v.pos.x);
        maxX = std::max(maxX, v.pos.x);
    }
    CHECK(maxX - minX > 0.0f);
    sceneops::cancelTool(s, h, t);
    CHECK(test::sameMesh(obj.mesh, test::meshFromRaw(pre)));

    // Infinite touch rejected for move too.
    REQUIRE(sceneops::beginTool(s, h, t, sceneops::Tool::Move, 1, 0, 400.0f, 300.0f) ==
            ops::Status::OK);
    CHECK(sceneops::updateTool(s, t, map,
                               std::numeric_limits<float>::infinity(), 300.0f) ==
          ops::Status::ErrInvalidArg);
    CHECK(test::sameMesh(obj.mesh, test::meshFromRaw(pre)));
    sceneops::cancelTool(s, h, t);

    // Mapping-level clamps: the accum helper never leaves the safe band.
    CHECK(ops::clampAccumScale(0.0f) >= ops::kMinAccumScaleFactor);
    CHECK(ops::clampAccumScale(-5.0f) >= ops::kMinAccumScaleFactor);
    CHECK(ops::clampAccumScale(1e9f) <= ops::kMaxAccumScaleFactor);
    CHECK(ops::clampAccumScale(std::numeric_limits<float>::quiet_NaN()) == 1.0f);
    CHECK(ops::clampAccumScale(2.0f) == 2.0f); // sane values pass through

    // Axial scale X ×2 then ×0.5 restores the start geometry exactly (a
    // normal scale never flattens the mesh by accident).
    REQUIRE(sceneops::beginTool(s, h, t, sceneops::Tool::Scale, 1, 0, 400.0f, 300.0f) ==
            ops::Status::OK);
    CHECK(sceneops::updateTool(s, t, map, 550.0f, 300.0f) == ops::Status::OK); // ×2
    CHECK(sceneops::updateTool(s, t, map, 475.0f, 300.0f) == ops::Status::OK); // ×0.5
    CHECK(test::sameMesh(obj.mesh, test::meshFromRaw(pre)));
    sceneops::cancelTool(s, h, t);
}

// ------------------------------------------- extrusion winding / normals ----

// After a cube-top extrusion every face normal points AWAY from the mesh
// center (culling-safe outward winding) and stays finite.
void testExtrudeWindingOutward() {
    Scene s;
    History h;
    REQUIRE(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    h.clear();
    SceneObject& obj = s.objects[0];
    s.editMode = true;

    const ElemId top = topFaceOf(obj.mesh);
    REQUIRE(top != kNull);
    REQUIRE(sceneops::selectFromPick(s, 3, 0, top) == ops::Status::OK);
    REQUIRE(sceneops::extrude(s, h) == ops::Status::OK);
    REQUIRE(obj.mesh.validate());

    // The documented extrusion creates the cap AT THE SAME position (walls
    // are degenerate until the user moves it). Pull the caps up 1 unit so
    // the walls gain real area — then the winding check is meaningful.
    for (const ElemId f : obj.sel.faces) {
        for (const ElemId v : obj.mesh.faceVertices(f)) {
            obj.mesh.vrt(v).pos.y += 1.0f;
        }
    }

    Vec3 center{};
    size_t n = 0;
    for (const auto& v : obj.mesh.vertices()) {
        if (!v.alive) continue;
        center = center + v.pos;
        ++n;
    }
    center = center * (1.0f / static_cast<float>(n));

    int outward = 0;
    int total = 0;
    bool allFinite = true;
    for (const ElemId f : obj.mesh.aliveFaceIds()) {
        const Vec3 nrm = obj.mesh.faceNormal(f);
        if (!ops::isFiniteVec(nrm.x, nrm.y, nrm.z)) allFinite = false;
        const Vec3 c = obj.mesh.faceCentroid(f) - center;
        if (dot(nrm, c) > 0.0f) ++outward;
        ++total;
    }
    CHECK(allFinite);
    CHECK(total == 10);       // 5 remaining original + 4 walls + 1 cap
    CHECK(outward == total);  // EVERY face reads from the outside

    // The render buffers reflect the updated mesh (flat shading, 10 quads).
    renderdata::RenderData rd;
    renderdata::buildObject(obj, rd);
    CHECK(test::assertNormalsOk(obj.mesh));
    CHECK(rd.triangleCount == 20);
    CHECK(rd.fillVertexCount == 40);
    CHECK(rd.editableVertexCount == 12); // 8 original + 4 duplicated cap verts
}

// ------------------------------------------------------- undo/redo + sel ----

// A full edit session (transform -> extrude -> undo x2 -> redo x2 -> undo)
// always ends with an alive-only selection and a valid mesh.
void testSelectionCoherentThroughHistoryStorm() {
    Scene s;
    History h;
    REQUIRE(sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0) >= 0);
    h.clear(); // the ObjAdd entry is not part of this storm
    SceneObject& obj = s.objects[0];
    s.editMode = true;

    const auto checkCoherent = [&] {
        for (const ElemId v : obj.sel.verts) CHECK(obj.mesh.vertexAlive(v));
        for (const ElemId e : obj.sel.edges) CHECK(obj.mesh.edgeAlive(e));
        for (const ElemId f : obj.sel.faces) CHECK(obj.mesh.faceAlive(f));
        CHECK(obj.mesh.validate());
    };

    REQUIRE(sceneops::selectFromPick(s, 3, 0, obj.mesh.aliveFaceIds()[0]) == ops::Status::OK);
    checkCoherent();

    // Transform via the gizmo session (history entry 1).
    sceneops::ToolSession t;
    sceneops::GizmoMapping map;
    map.axisDirX = 1.0f;
    map.axisDirY = 0.0f;
    map.pxPerWorld = 100.0f;
    map.pivotScreenX = 0.0f;
    map.pivotScreenY = 0.0f;
    map.ringSign = 1.0f;
    REQUIRE(sceneops::beginTool(s, h, t, sceneops::Tool::Move, 1, 0, 0.0f, 0.0f) ==
            ops::Status::OK);
    CHECK(sceneops::updateTool(s, t, map, 100.0f, 0.0f) == ops::Status::OK);
    REQUIRE(sceneops::endTool(s, h, t) == ops::Status::OK);
    checkCoherent();

    // Extrude (history entry 2 — selection moves to caps).
    REQUIRE(sceneops::extrude(s, h) == ops::Status::OK);
    checkCoherent();

    for (int i = 0; i < 2; ++i) {
        REQUIRE(sceneops::undo(s, h) == ops::Status::OK);
        checkCoherent();
    }
    for (int i = 0; i < 2; ++i) {
        REQUIRE(sceneops::redo(s, h) == ops::Status::OK);
        checkCoherent();
    }
    // Undo back to the start: the face selection is restored alive-only.
    REQUIRE(sceneops::undo(s, h) == ops::Status::OK);
    checkCoherent();
    CHECK(!obj.sel.faces.empty());
    for (const ElemId f : obj.sel.faces) CHECK(obj.mesh.faceAlive(f));
}

} // namespace

namespace blone::test {
void runSelectionStabilityTests() {
    testEmptySelectionProducesNoOverlay();
    testStaleIdsAreRejectedEverywhere();
    testSelectionValidAfterOperations();
    testScaleRejectionAndClamps();
    testExtrudeWindingOutward();
    testSelectionCoherentThroughHistoryStorm();
}
} // namespace blone::test
