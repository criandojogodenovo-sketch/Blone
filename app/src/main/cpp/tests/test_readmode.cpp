// F4-LIMITS P3: READ-ONLY objects (RenderMesh) — the host-testable contracts:
//  - RenderMesh build / memory / round-trip (promotion is bit-exact);
//  - read objects reject sculpt/paint/selection (the brush and edit picks
//    skip them; the sceneops layer guards the rest);
//  - the OBJECT gizmo works on read objects (Move via obj.position,
//    Rotate/Scale rejected ErrUnsupported);
//  - promotion through replaceWithEditable: ONE ObjectSwap entry, undo
//    restores the read object, redo the editable one — geometry preserved
//    bit-exact both ways.
#include "test_util.h"

#include <cmath>

#include "../core/history/History.h"
#include "../core/io/GeometryLimits.h"
#include "../core/io/MeshBuilder.h"
#include "../core/mesh/Primitives.h"
#include "../core/render/RenderMesh.h"
#include "../core/scene/SceneOps.h"

namespace {

using blone::ElemId;
using blone::HalfEdgeMesh;
using blone::History;
using blone::PrimType;
using blone::Scene;
using blone::SceneObject;
using blone::Vec3;
using blone::ops::Status;
using blone::io::MeshBuilder;
using blone::io::RawMeshData;
namespace sceneops = blone::sceneops;

// A small quad mesh as the loader would produce it (2 triangles).
RawMeshData quadRaw() {
    RawMeshData raw;
    raw.positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0},
                     Vec3{0, 1, 0}};
    raw.colors = {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1},
                  Vec3{1, 1, 0}};
    raw.faceIndices.push_back({0, 1, 2});
    raw.faceIndices.push_back({0, 2, 3});
    return raw;
}

SceneObject makeReadObject(const RawMeshData& raw, const char* name) {
    SceneObject obj;
    obj.name = name;
    obj.editable = false;
    obj.readMesh = std::make_shared<const blone::render::RenderMesh>(
            blone::render::buildRenderMesh(raw));
    return obj;
}

// ------------------------------------------------------------ 1 -----------
void testRenderMeshBuildAndRoundTrip() {
    const RawMeshData raw = quadRaw();
    const blone::render::RenderMesh rm = blone::render::buildRenderMesh(raw);
    CHECK(!rm.empty());
    CHECK(rm.positions.size() == 4);
    CHECK(rm.triangleCount == 2);
    CHECK(rm.indices.size() == 6);
    // Positions/colors copied verbatim (bit-exact promotion contract).
    for (size_t i = 0; i < raw.positions.size(); ++i) {
        CHECK(rm.positions[i].x == raw.positions[i].x);
        CHECK(rm.positions[i].y == raw.positions[i].y);
        CHECK(rm.positions[i].z == raw.positions[i].z);
        CHECK(rm.colors[i].x == raw.colors[i].x);
    }
    // Bounds and centroid of the unit quad.
    CHECK_NEAR(rm.bounds.minX, 0.0f, 1e-6f);
    CHECK_NEAR(rm.bounds.maxX, 1.0f, 1e-6f);
    CHECK_NEAR(rm.centroid.x, 0.5f, 1e-6f);
    CHECK_NEAR(rm.centroid.y, 0.5f, 1e-6f);
    // memoryBytes covers the three arrays (documentation + budget).
    CHECK(rm.memoryBytes() >=
          static_cast<uint64_t>(rm.positions.capacity() * sizeof(Vec3) +
                                rm.colors.capacity() * sizeof(Vec3) +
                                rm.indices.capacity() * sizeof(uint32_t)));

    // Round-trip raw -> RenderMesh -> raw -> MeshBuilder == direct build.
    const blone::io::RawMeshData back = blone::render::renderMeshToRaw(rm);
    CHECK(back.positions.size() == raw.positions.size());
    auto direct = MeshBuilder::build(raw);
    auto viaRead = MeshBuilder::build(back);
    REQUIRE(direct.has_value());
    REQUIRE(viaRead.has_value());
    CHECK(direct->aliveVertexCount() == viaRead->aliveVertexCount());
    CHECK(direct->aliveFaceCount() == viaRead->aliveFaceCount());
    // Promotion round-trip: the built mesh positions are BIT-EXACT to the
    // RenderMesh source (verbatim copy both ways).
    const blone::io::RawMeshData raw2 = quadRaw();
    auto mesh = MeshBuilder::build(raw2);
    REQUIRE(mesh.has_value());
    REQUIRE(mesh->aliveVertexCount() == rm.positions.size());
    size_t i = 0;
    for (const auto& v : mesh->vertices()) {
        if (!v.alive) continue;
        if (i >= rm.positions.size()) break;
        CHECK(v.pos.x == rm.positions[i].x);
        CHECK(v.pos.y == rm.positions[i].y);
        CHECK(v.pos.z == rm.positions[i].z);
        ++i;
    }
}

// ------------------------------------------------------------ 2 -----------
void testReadObjectRejectsEditing() {
    Scene s;
    History h;
    s.objects.push_back(makeReadObject(quadRaw(), "leitura"));
    s.active = 0;
    s.bump();

    // No selection can exist on a read object (edit picks skip them) and
    // the modeling operations fail with ErrNoSelection (empty mesh).
    CHECK(!sceneops::hasValidSelection(s));
    CHECK(sceneops::extrude(s, h) == Status::ErrNoSelection);
    CHECK(sceneops::subdivide(s, h) == Status::ErrNoSelection);
    CHECK(sceneops::deleteSelection(s, h) == Status::ErrNoSelection);
    CHECK(sceneops::loopCut(s, h) == Status::ErrNoSelection);
    // History untouched by the rejections.
    CHECK(h.undoDepth() == 0);
    // Object intact.
    REQUIRE(s.objects.size() == 1);
    CHECK(!s.objects[0].editable);
    CHECK(s.objects[0].mesh.empty());
}

// ------------------------------------------------------------ 3 -----------
void testReadObjectBrushAndEditPicks() {
    // The brush never targets read objects: surfaceHitAt skips them, so
    // beginBrushStroke has no hit surface (checked at the sceneops level:
    // even a direct call with a fabricated hit would mutate nothing — here
    // we pin the scene-side guard that the session cannot start).
    Scene s;
    History h;
    s.objects.push_back(makeReadObject(quadRaw(), "leitura"));
    s.active = 0;
    s.bump();
    sceneops::BrushSession b;
    const blone::brush::BrushSettings settings;
    // A fabricated LOCAL hit (any point): the sceneops begin must reject —
    // the object has NO half-edge faces to sculpt.
    CHECK(sceneops::beginBrushStroke(s, h, b, sceneops::BrushTool::Sculpt,
                                     0, Vec3{0.5f, 0.0f, 0.5f},
                                     Vec3{0, 1, 0}, settings) !=
          Status::OK);
    CHECK(!b.active);
    // Paint likewise.
    CHECK(sceneops::beginBrushStroke(s, h, b, sceneops::BrushTool::Paint,
                                     0, Vec3{0.5f, 0.0f, 0.5f},
                                     Vec3{0, 1, 0}, settings) !=
          Status::OK);
    CHECK(!b.active);
}

// ------------------------------------------------------------ 4 -----------
void testReadObjectGizmoMoveOnly() {
    Scene s;
    History h;
    s.objects.push_back(makeReadObject(quadRaw(), "leitura"));
    s.active = 0;
    s.bump();

    // OBJECT-mode Move gizmo works: session on the read object moves ONLY
    // obj.position (the geometry is untouched and nothing commits).
    sceneops::ToolSession t;
    CHECK(sceneops::beginTool(s, h, t, sceneops::Tool::Move, 1, 0,
                              100.0f, 100.0f) == Status::OK);
    CHECK(t.active);
    CHECK(t.readMode);
    sceneops::GizmoMapping map; // identity mapping: 1 px = 1 world unit
    map.axisDirX = 1.0f;
    map.axisDirY = 0.0f;
    map.pxPerWorld = 1.0f;
    const Vec3 pos0 = s.objects[0].position;
    CHECK(sceneops::updateTool(s, t, map, 140.0f, 100.0f) == Status::OK);
    const Vec3 pos1 = s.objects[0].position;
    CHECK(pos1.x > pos0.x + 1e-4f); // moved along +X
    CHECK(s.objects[0].mesh.empty());
    CHECK(sceneops::endTool(s, h, t) == Status::OK);
    CHECK(h.undoDepth() == 0); // read-only transforms are not undoable
    CHECK(s.objects[0].position.x > pos0.x);

    // Rotate gizmo: rejected (ErrUnsupported) — nothing starts.
    sceneops::ToolSession t2;
    CHECK(sceneops::beginTool(s, h, t2, sceneops::Tool::Rotate, 2, 0,
                              100.0f, 100.0f) == Status::ErrUnsupported);
    CHECK(!t2.active);
    // Scale gizmo (center handle): rejected.
    sceneops::ToolSession t3;
    CHECK(sceneops::beginTool(s, h, t3, sceneops::Tool::Scale, 3, 3,
                              100.0f, 100.0f) == Status::ErrUnsupported);
    CHECK(!t3.active);

    // Direct steps from the properties panel: Move works via position,
    // Rotate/Scale are rejected.
    CHECK(sceneops::transformDirect(s, h, sceneops::Tool::Move, 1, 0.25f) ==
          Status::OK);
    CHECK(s.objects[0].position.y > pos1.y + 1e-4f);
    CHECK(sceneops::transformDirect(s, h, sceneops::Tool::Rotate, 0, 15.0f) ==
          Status::ErrUnsupported);
    CHECK(sceneops::transformDirect(s, h, sceneops::Tool::Scale, 3, 1.25f) ==
          Status::ErrUnsupported);

    // Cancellation restores the pre-drag position.
    sceneops::ToolSession t4;
    const Vec3 posNow = s.objects[0].position;
    CHECK(sceneops::beginTool(s, h, t4, sceneops::Tool::Move, 1, 2,
                              100.0f, 100.0f) == Status::OK);
    CHECK(sceneops::updateTool(s, t4, map, 100.0f, 160.0f) == Status::OK);
    sceneops::cancelTool(s, h, t4);
    CHECK(s.objects[0].position.z == posNow.z);
}

void iotaCheck(const HalfEdgeMesh& m) {
    // The promoted mesh keeps the verbatim quad geometry.
    bool found = false;
    for (const auto& v : m.vertices()) {
        if (v.alive && v.pos.x == 1.0f && v.pos.y == 1.0f) {
            found = true;
        }
    }
    CHECK(found);
}

// ------------------------------------------------------------ 5 -----------
void testPromotionRoundTripWithHistory() {
    Scene s;
    History h;
    s.objects.push_back(makeReadObject(quadRaw(), "leitura"));
    s.active = 0;
    s.bump();

    // Build the editable replacement the way the worker does: verbatim raw
    // -> MeshBuilder, keeping name/position.
    const blone::io::RawMeshData raw = quadRaw();
    auto mesh = MeshBuilder::build(raw);
    REQUIRE(mesh.has_value());
    SceneObject replacement;
    replacement.name = s.objects[0].name;
    replacement.position = s.objects[0].position;
    replacement.editable = true;
    replacement.mesh = std::move(*mesh);

    CHECK(sceneops::replaceWithEditable(s, h, 0, std::move(replacement)) ==
          Status::OK);
    CHECK(s.objects[0].editable);
    CHECK(s.objects[0].readMesh == nullptr);
    CHECK(s.objects[0].mesh.aliveVertexCount() == 4);
    CHECK(h.undoDepth() == 1); // EXACTLY ONE entry (mission requirement)
    CHECK(h.redoDepth() == 0);

    // UNDO restores the read-only object (same shared geometry).
    CHECK(sceneops::undo(s, h) == Status::OK);
    REQUIRE(s.objects.size() == 1);
    CHECK(!s.objects[0].editable);
    REQUIRE(s.objects[0].readMesh != nullptr);
    CHECK(s.objects[0].readMesh->triangleCount == 2);
    CHECK(h.redoDepth() == 1);

    // REDO restores the editable object — bit-exact geometry.
    CHECK(sceneops::redo(s, h) == Status::OK);
    REQUIRE(s.objects.size() == 1);
    CHECK(s.objects[0].editable);
    CHECK(s.objects[0].mesh.aliveVertexCount() == 4);
    iotaCheck(s.objects[0].mesh);
}

// ------------------------------------------------------------ 6 -----------
void testReadImportLifecycle() {
    Scene s;
    History h;
    // Read import via the facade: one ObjAdd entry, object active.
    SceneObject obj = makeReadObject(quadRaw(), "importado");
    CHECK(sceneops::addImportedReadObject(s, h, std::move(obj)) == Status::OK);
    REQUIRE(s.objects.size() == 1);
    CHECK(s.active == 0);
    CHECK(!s.objects[0].editable);
    CHECK(h.undoDepth() == 1);
    // Undo removes the import entirely; redo restores it.
    CHECK(sceneops::undo(s, h) == Status::OK);
    CHECK(s.objects.empty());
    CHECK(sceneops::redo(s, h) == Status::OK);
    REQUIRE(s.objects.size() == 1);
    CHECK(!s.objects[0].editable);
    REQUIRE(s.objects[0].readMesh != nullptr);
    CHECK(s.objects[0].readMesh->triangleCount == 2);

    // Duplicating a read object shares the geometry (another read object).
    CHECK(sceneops::duplicateActiveObject(s, h) == 1);
    REQUIRE(s.objects.size() == 2);
    CHECK(!s.objects[1].editable);
    CHECK(s.objects[1].readMesh == s.objects[0].readMesh); // shared, not copied
    CHECK(s.objects[1].position.x == s.objects[0].position.x + 1.5f);

    // Deleting a read object works (one ObjRemove entry).
    s.active = 1;
    CHECK(sceneops::deleteActiveObject(s, h) == Status::OK);
    REQUIRE(s.objects.size() == 1);
    CHECK(!s.objects[0].editable);

    // Contract guards: an editable object cannot enter via the read door.
    SceneObject wrong;
    wrong.name = "editavel";
    wrong.editable = true;
    CHECK(sceneops::addImportedReadObject(s, h, std::move(wrong)) ==
          Status::ErrInvalidArg);
}

// ------------------------------------------------------------ 7 -----------
void testPromotionGuardrails() {
    Scene s;
    History h;
    // Promotion of an EDITABLE object is rejected.
    const int idx = sceneops::createPrimitive(s, h, PrimType::Cube, 0, 0);
    REQUIRE(idx == 0);
    SceneObject replacement;
    replacement.name = "x";
    replacement.editable = true;
    HalfEdgeMesh m;
    blone::PrimitiveParams p;
    REQUIRE(blone::buildPrimitive(PrimType::Cube, p, m));
    replacement.mesh = std::move(m);
    CHECK(sceneops::replaceWithEditable(s, h, 0, std::move(replacement)) ==
          Status::ErrInvalidArg);
    // Out-of-range index is rejected.
    SceneObject r2;
    r2.editable = true;
    HalfEdgeMesh m2;
    REQUIRE(blone::buildPrimitive(PrimType::Cube, p, m2));
    r2.mesh = std::move(m2);
    CHECK(sceneops::replaceWithEditable(s, h, 5, std::move(r2)) ==
          Status::ErrInvalidArg);
    // Promotion above the profile edit limits fails BEFORE the build —
    // pinned at the classify level (the worker path):
    const auto lim = blone::io::limitsFor(blone::io::LimitsProfile::Baixo);
    CHECK(blone::io::classifyImport(40001, 100, lim) ==
          blone::io::ImportClass::ReadOnly); // verts above edit budget
    CHECK(blone::io::classifyImport(10, 80001, lim) ==
          blone::io::ImportClass::ReadOnly); // tris above edit budget
}

} // namespace

namespace blone::test {

void runReadModeTests() {
    std::printf("-- modo leitura (RenderMesh, promoção) --\n");
    testRenderMeshBuildAndRoundTrip();
    testReadObjectRejectsEditing();
    testReadObjectBrushAndEditPicks();
    testReadObjectGizmoMoveOnly();
    testPromotionRoundTripWithHistory();
    testReadImportLifecycle();
    testPromotionGuardrails();
}

} // namespace blone::test
