#include "SceneOps.h"

#include "../Log.h"

#include <algorithm>
#include <cmath>

namespace blone::sceneops {

namespace {

// Conservative history bounds per operation kind (multiplier over the
// pre-state bytes; the exact entry cost replaces the bound on commit).
constexpr uint64_t kBoundExtrude = 3;   // pre + post + slack
constexpr uint64_t kBoundDelete = 3;    // post is smaller, bound is safe
constexpr uint64_t kBoundLoopCut = 3;
constexpr uint64_t kBoundSubdivide = 6; // faces/verts grow up to ~4x

Selection::Set& setFor(SceneObject& obj, SelMode mode) {
    switch (mode) {
        case SelMode::Vertex: return obj.sel.verts;
        case SelMode::Edge: return obj.sel.edges;
        case SelMode::Face: return obj.sel.faces;
    }
    return obj.sel.faces;
}

std::vector<ElemId> toVector(const Selection::Set& set) {
    return std::vector<ElemId>(set.begin(), set.end());
}

} // namespace

// ---------------------------------------------------- object management ----

int createPrimitive(Scene& s, History& h, PrimType type, int segsU, int segsV) {
    if (s.objects.size() >= Scene::kMaxObjects) return -static_cast<int>(Status::ErrLimit);

    // Deterministic placement on the grid, away from other objects.
    const int n = static_cast<int>(s.objects.size());
    PrimitiveParams p;
    p.cx = 3.0f * static_cast<float>(n % 6);
    p.cz = -3.0f * static_cast<float>((n / 6) % 6);
    p.cy = (type == PrimType::Plane) ? 0.0f : 1.0f;
    p.size = 1.0f;
    p.segsU = segsU;
    p.segsV = segsV;

    // Build into a temporary mesh first (scene untouched on failure).
    SceneObject obj;
    obj.position = Vec3{0.0f, 0.0f, 0.0f};
    switch (type) {
        case PrimType::Cube: obj.name = "Cubo"; break;
        case PrimType::Sphere: obj.name = "Esfera"; break;
        case PrimType::Cylinder: obj.name = "Cilindro"; break;
        case PrimType::Plane: obj.name = "Plano"; break;
    }
    if (s.objects.size() < 8) {
        // Keep the first object at the origin for a predictable start.
        if (n == 0) {
            p.cx = 0.0f;
            p.cz = 0.0f;
        }
    }
    if (!buildPrimitive(type, p, obj.mesh)) return -static_cast<int>(Status::ErrLimit);

    // History reservation BEFORE the scene changes (condition 3).
    const uint64_t cost = obj.mesh.rawBytes() * 2 + 256;
    if (!h.reserve(cost)) return -static_cast<int>(Status::ErrBudget);

    s.objects.push_back(std::move(obj));
    const int index = static_cast<int>(s.objects.size()) - 1;
    s.active = index;
    if (!h.pushObjectAdded(static_cast<uint32_t>(index), s.objects[static_cast<size_t>(index)])) {
        h.rollbackReservation();
        return -static_cast<int>(Status::ErrInternal);
    }
    s.bump();
    return index;
}

Status addImportedObject(Scene& s, History& h, SceneObject&& obj) {
    if (s.objects.size() >= Scene::kMaxObjects) return Status::ErrLimit;
    if (obj.mesh.empty()) return Status::ErrInvalidArg;

    // History reservation BEFORE the scene changes (same contract as
    // createPrimitive). One ObjectAdded entry: undo removes the import.
    const uint64_t cost = obj.mesh.rawBytes() * 2 + 256;
    if (!h.reserve(cost)) return Status::ErrBudget;

    obj.position = Vec3{0.0f, 0.0f, 0.0f}; // files carry world coordinates
    obj.sel.clear();
    s.objects.push_back(std::move(obj));
    const uint32_t index = static_cast<uint32_t>(s.objects.size() - 1);
    if (!h.pushObjectAdded(index, s.objects[index])) {
        // Push failed: restore the pre-mutation state exactly (stricter
        // than createPrimitive — the object leaves the scene again).
        h.rollbackReservation();
        s.objects.pop_back();
        return Status::ErrInternal;
    }
    s.active = static_cast<int>(index);
    pruneAllSelections(s); // defensive hygiene, mirrors the JNI drain
    s.bump();              // renderer resync on the next frame
    return Status::OK;
}

Status deleteActiveObject(Scene& s, History& h) {
    SceneObject* obj = s.activeObject();
    if (!obj) return Status::ErrNoActiveObject;
    const int index = s.active;
    const uint64_t cost = obj->mesh.rawBytes() * 2 + 256;
    if (!h.reserve(cost)) return Status::ErrBudget;
    SceneObject copy = std::move(*obj);
    s.objects.erase(s.objects.begin() + index);
    s.active = std::min<int>(s.active, static_cast<int>(s.objects.size()) - 1);
    if (!h.pushObjectRemoved(static_cast<uint32_t>(index), copy)) {
        h.rollbackReservation();
        return Status::ErrInternal;
    }
    s.bump();
    return Status::OK;
}

int duplicateActiveObject(Scene& s, History& h) {
    SceneObject* obj = s.activeObject();
    if (!obj) return -static_cast<int>(Status::ErrNoActiveObject);
    if (s.objects.size() >= Scene::kMaxObjects) return -static_cast<int>(Status::ErrLimit);

    // Build the clone into a temporary first: the scene is untouched on
    // failure (same contract as createPrimitive).
    SceneObject clone;
    clone.name = obj->name;
    // Visible offset so the copy is distinguishable from the original.
    clone.position = obj->position + Vec3{1.5f, 0.0f, 0.0f};
    clone.mesh.restoreRaw(obj->mesh.captureRaw());
    clone.sel.clear();

    // History reservation BEFORE the scene changes (condition 3).
    const uint64_t cost = clone.mesh.rawBytes() * 2 + 256;
    if (!h.reserve(cost)) return -static_cast<int>(Status::ErrBudget);

    s.objects.push_back(std::move(clone));
    const int index = static_cast<int>(s.objects.size()) - 1;
    s.active = index;
    if (!h.pushObjectAdded(static_cast<uint32_t>(index), s.objects[static_cast<size_t>(index)])) {
        h.rollbackReservation();
        return -static_cast<int>(Status::ErrInternal);
    }
    s.bump();
    return index;
}

Status setActiveObject(Scene& s, int index) {
    if (index < 0 || index >= static_cast<int>(s.objects.size())) return Status::ErrInvalidArg;
    s.active = index;
    s.bump();
    return Status::OK;
}

Status setEditMode(Scene& s, bool edit) { s.editMode = edit; s.bump(); return Status::OK; }
Status setSelectionMode(Scene& s, SelMode mode) { s.selectionMode = mode; s.bump(); return Status::OK; }
Status setMultiSelect(Scene& s, bool on) { s.multiSelect = on; s.bump(); return Status::OK; }

Status clearSelection(Scene& s) {
    SceneObject* obj = s.activeObject();
    if (!obj) return Status::ErrNoActiveObject;
    obj->sel.clear();
    s.bump();
    return Status::OK;
}

// ---- selection state queries (single source of truth) ---------------------

bool hasValidSelection(const Scene& s) {
    const SceneObject* obj = s.activeObject();
    if (!obj) return false;
    for (const ElemId v : obj->sel.verts) {
        if (obj->mesh.vertexAlive(v)) return true;
    }
    for (const ElemId e : obj->sel.edges) {
        if (obj->mesh.edgeAlive(e)) return true;
    }
    for (const ElemId f : obj->sel.faces) {
        if (obj->mesh.faceAlive(f)) return true;
    }
    return false;
}

void selectionCounts(const Scene& s, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    const SceneObject* obj = s.activeObject();
    if (!obj) return;
    for (const ElemId v : obj->sel.verts) {
        if (obj->mesh.vertexAlive(v)) ++out[0];
    }
    for (const ElemId e : obj->sel.edges) {
        if (obj->mesh.edgeAlive(e)) ++out[1];
    }
    for (const ElemId f : obj->sel.faces) {
        if (obj->mesh.faceAlive(f)) ++out[2];
    }
}

bool pruneAllSelections(Scene& s) {
    bool changed = false;
    for (SceneObject& obj : s.objects) {
        const size_t before = obj.sel.verts.size() + obj.sel.edges.size() +
                              obj.sel.faces.size();
        obj.sel.prune(obj.mesh);
        const size_t after = obj.sel.verts.size() + obj.sel.edges.size() +
                             obj.sel.faces.size();
        if (after != before) changed = true;
    }
    return changed;
}

Status selectFromPick(Scene& s, int kind, int objIndex, ElemId elemId) {
    if (objIndex < 0 || objIndex >= static_cast<int>(s.objects.size())) {
        return Status::ErrInvalidArg;
    }
    if (kind == 4) { // object selection / activation
        return setActiveObject(s, objIndex);
    }
    if (!s.editMode) return Status::ErrInvalidArg;
    SceneObject& obj = s.objects[static_cast<size_t>(objIndex)];
    s.active = objIndex;
    Selection::Set* set = nullptr;
    bool alive = false;
    switch (kind) {
        case 1: set = &obj.sel.verts; alive = obj.mesh.vertexAlive(elemId); break;
        case 2: set = &obj.sel.edges; alive = obj.mesh.edgeAlive(elemId); break;
        case 3: set = &obj.sel.faces; alive = obj.mesh.faceAlive(elemId); break;
        default: return Status::ErrInvalidArg;
    }
    if (!alive) return Status::ErrInvalidArg;
    if (!s.multiSelect) {
        // Single-select: replace the whole selection.
        obj.sel.clear();
        set->insert(elemId);
    } else {
        if (!set->insert(elemId).second) set->erase(elemId); // toggle
    }
    s.bump();
    return Status::OK;
}

// ---------------------------------------------------- modeling operations --

Status extrude(Scene& s, History& h) {
    SceneObject* obj = s.activeObject();
    if (!obj) return Status::ErrNoActiveObject;
    std::vector<ElemId> faces = toVector(obj->sel.faces);
    if (faces.empty()) return Status::ErrNoSelection;

    const HalfEdgeMesh::Raw pre = obj->mesh.captureRaw();
    const Selection selBefore = obj->sel;
    const uint64_t bound = obj->mesh.rawBytes() * kBoundExtrude + 4096;
    if (!h.reserve(bound)) return Status::ErrBudget;

    std::set<ElemId> caps;
    const Status st = ops::extrudeFaces(obj->mesh, faces, caps);
    if (st != Status::OK) {
        h.rollbackReservation();
        return st; // mesh untouched: ops:: rolled back internally
    }
    // The selection moves to the new cap faces (single source of truth: the
    // engine decides; the entry restores it on undo/redo).
    Selection selAfter;
    selAfter.faces = caps;
    if (!h.pushMeshSwapWithSelection(static_cast<uint32_t>(s.active), pre,
                                     obj->mesh.captureRaw(), selBefore, selAfter)) {
        obj->mesh.restoreRaw(pre);
        obj->sel = selBefore;
        h.rollbackReservation();
        return Status::ErrInternal;
    }
    obj->sel = selAfter;
    s.bump();
    return Status::OK;
}

Status subdivide(Scene& s, History& h) {
    SceneObject* obj = s.activeObject();
    if (!obj) return Status::ErrNoActiveObject;
    std::vector<ElemId> faces = toVector(obj->sel.faces);
    // No face selection: subdivide the whole mesh (documented behaviour).
    if (faces.empty()) {
        faces = obj->mesh.aliveFaceIds();
        if (faces.empty()) return Status::ErrNoSelection;
    }

    const HalfEdgeMesh::Raw pre = obj->mesh.captureRaw();
    const Selection selBefore = obj->sel;
    const uint64_t bound = obj->mesh.rawBytes() * kBoundSubdivide + 4096;
    if (!h.reserve(bound)) return Status::ErrBudget;

    const Status st = ops::subdivideFaces(obj->mesh, faces);
    if (st != Status::OK) {
        h.rollbackReservation();
        return st;
    }
    const Selection selAfter; // subdivide always clears the selection
    if (!h.pushMeshSwapWithSelection(static_cast<uint32_t>(s.active), pre,
                                     obj->mesh.captureRaw(), selBefore, selAfter)) {
        obj->mesh.restoreRaw(pre);
        obj->sel = selBefore;
        h.rollbackReservation();
        return Status::ErrInternal;
    }
    obj->sel = selAfter;
    s.bump();
    return Status::OK;
}

Status deleteSelection(Scene& s, History& h) {
    SceneObject* obj = s.activeObject();
    if (!obj) return Status::ErrNoActiveObject;
    const std::vector<ElemId> sel = toVector(setFor(*obj, s.selectionMode));
    if (sel.empty()) return Status::ErrNoSelection;

    const HalfEdgeMesh::Raw pre = obj->mesh.captureRaw();
    const Selection selBefore = obj->sel;
    const uint64_t bound = obj->mesh.rawBytes() * kBoundDelete + 4096;
    if (!h.reserve(bound)) return Status::ErrBudget;

    ops::DelKind kind = ops::DelKind::Faces;
    switch (s.selectionMode) {
        case SelMode::Vertex: kind = ops::DelKind::Vertices; break;
        case SelMode::Edge: kind = ops::DelKind::Edges; break;
        case SelMode::Face: kind = ops::DelKind::Faces; break;
    }
    const Status st = ops::deleteElements(obj->mesh, kind, sel);
    if (st != Status::OK) {
        h.rollbackReservation();
        return st;
    }
    Selection selAfter = selBefore;
    selAfter.prune(obj->mesh); // survivors only
    if (!h.pushMeshSwapWithSelection(static_cast<uint32_t>(s.active), pre,
                                     obj->mesh.captureRaw(), selBefore, selAfter)) {
        obj->mesh.restoreRaw(pre);
        obj->sel = selBefore;
        h.rollbackReservation();
        return Status::ErrInternal;
    }
    obj->sel = selAfter;
    s.bump();
    return Status::OK;
}

Status loopCut(Scene& s, History& h) {
    SceneObject* obj = s.activeObject();
    if (!obj) return Status::ErrNoActiveObject;
    const std::vector<ElemId> edges = toVector(obj->sel.edges);
    if (edges.empty()) return Status::ErrNoSelection;

    const HalfEdgeMesh::Raw pre = obj->mesh.captureRaw();
    const Selection selBefore = obj->sel;
    const uint64_t bound = obj->mesh.rawBytes() * kBoundLoopCut + 4096;
    if (!h.reserve(bound)) return Status::ErrBudget;

    ops::LoopCutReport rep;
    const Status st = ops::loopCut(obj->mesh, edges.front(), rep);
    if (st != Status::OK) {
        h.rollbackReservation();
        return st;
    }
    const Selection selAfter; // loop cut clears the selection
    if (!h.pushMeshSwapWithSelection(static_cast<uint32_t>(s.active), pre,
                                     obj->mesh.captureRaw(), selBefore, selAfter)) {
        obj->mesh.restoreRaw(pre);
        obj->sel = selBefore;
        h.rollbackReservation();
        return Status::ErrInternal;
    }
    obj->sel = selAfter;
    s.bump();
    return Status::OK;
}

// -------------------------------------------------------- transform tools --

namespace {

Vec3 axisUnit(int a) {
    switch (a) {
        case 0: return Vec3{1.0f, 0.0f, 0.0f};
        case 1: return Vec3{0.0f, 1.0f, 0.0f};
        default: return Vec3{0.0f, 0.0f, 1.0f};
    }
}

} // namespace

Status beginTool(Scene& s, History& h, ToolSession& t, Tool tool, int kind,
                 int axis, float touchX, float touchY) {
    (void)h;
    if (t.active) cancelTool(s, h, t);
    SceneObject* obj = s.activeObject();
    if (!obj) return Status::ErrNoActiveObject;
    // OBJECT mode transforms the WHOLE object (interface reorganization:
    // DECISÃO C — the user can move/rotate/scale a full object without
    // entering edit mode). EDIT mode keeps the selection-based target.
    if (kind < 1 || kind > 3 || axis < 0 || axis > 3) return Status::ErrInvalidArg;

    // Deduplicated vertex set from the current selection.
    std::set<ElemId> verts;
    if (s.editMode) {
        for (const ElemId v : obj->sel.verts) verts.insert(v);
        for (const ElemId e : obj->sel.edges) {
            if (!obj->mesh.edgeAlive(e)) continue;
            const ElemId he = obj->mesh.edg(e).he;
            verts.insert(obj->mesh.heg(he).origin);
            verts.insert(obj->mesh.heg(obj->mesh.heg(he).twin).origin);
        }
        for (const ElemId f : obj->sel.faces) {
            for (const ElemId v : obj->mesh.faceVertices(f)) verts.insert(v);
        }
    }
    if (verts.empty()) {
        // No selection (or OBJECT mode): the WHOLE object is the transform
        // target — the tool never locks up (touch a gizmo axis, it moves).
        for (const auto& v : obj->mesh.vertices()) {
            if (v.alive) verts.insert(v.id);
        }
    }
    if (verts.empty()) return Status::ErrNoSelection;

    ToolSession session;
    session.active = true;
    session.tool = tool;
    session.objIndex = s.active;
    session.verts.assign(verts.begin(), verts.end());
    session.kind = kind;
    session.axis = axis;
    session.beginTouchX = touchX;
    session.beginTouchY = touchY;
    session.lastTouchX = touchX;
    session.lastTouchY = touchY;
    session.lastTouchValid = true;
    session.accumMove = Vec3{};
    session.accumFactor = 1.0f;
    session.accumDeg = 0.0f;
    Vec3 centroid{};
    for (const ElemId v : session.verts) centroid = centroid + obj->mesh.vrt(v).pos;
    session.pivot = centroid * (1.0f / static_cast<float>(session.verts.size()));
    session.startPos.reserve(session.verts.size());
    for (const ElemId v : session.verts) session.startPos.push_back(obj->mesh.vrt(v).pos);

    // History reservation BEFORE any mutation (condition 3).
    const uint64_t cost = static_cast<uint64_t>(session.verts.size()) * 2ull * 16ull + 256;
    if (!h.reserve(cost)) return Status::ErrBudget;

    t = std::move(session);
    return Status::OK;
}

Status updateTool(Scene& s, ToolSession& t, const GizmoMapping& map,
                  float touchX, float touchY) {
    if (!t.active) return Status::ErrInvalidArg;
    if (t.objIndex < 0 || t.objIndex >= static_cast<int>(s.objects.size())) {
        return Status::ErrNoActiveObject;
    }
    // Non-finite touch input is REJECTED before any state changes (the mesh
    // and the session stay valid — stability correction).
    if (!std::isfinite(touchX) || !std::isfinite(touchY)) {
        return Status::ErrInvalidArg;
    }
    if (!t.lastTouchValid) {
        t.lastTouchX = touchX;
        t.lastTouchY = touchY;
        t.lastTouchValid = true;
        return Status::OK;
    }
    SceneObject& obj = s.objects[static_cast<size_t>(t.objIndex)];
    const float dx = touchX - t.lastTouchX;
    const float dy = touchY - t.lastTouchY;
    t.lastTouchX = touchX;
    t.lastTouchY = touchY;
    if (dx == 0.0f && dy == 0.0f) return Status::OK;

    switch (t.tool) {
        case Tool::Move: {
            // Axis drag: absolute offset from the start positions.
            const ops::AxisScreen as{map.axisDirX, map.axisDirY, map.pxPerWorld};
            const float world = ops::dragAlongAxisWorld(as, dx, dy);
            t.accumMove = t.accumMove + axisUnit(t.axis) * world;
            for (size_t i = 0; i < t.verts.size(); ++i) {
                const ElemId v = t.verts[i];
                if (obj.mesh.vertexAlive(v)) {
                    obj.mesh.vrt(v).pos = t.startPos[i] + t.accumMove;
                }
            }
            break;
        }
        case Tool::Rotate: {
            // Ring drag: absolute accumulated rotation from the start pose.
            const float sweep = ops::ringSweepDeg(t.lastTouchX - dx, t.lastTouchY - dy,
                                                  touchX, touchY,
                                                  map.pivotScreenX, map.pivotScreenY);
            t.accumDeg += map.ringSign * sweep;
            const Mat4 r = Mat4::rotationAxisDeg(axisUnit(t.axis), t.accumDeg);
            for (size_t i = 0; i < t.verts.size(); ++i) {
                const ElemId v = t.verts[i];
                if (!obj.mesh.vertexAlive(v)) continue;
                const Vec3 rel = t.startPos[i] - t.pivot;
                obj.mesh.vrt(v).pos = t.pivot + r.transformPoint(rel);
            }
            break;
        }
        case Tool::Scale: {
            if (t.kind == 3) { // center handle: uniform, absolute from begin
                const float f = ops::scaleFromDrag(touchY - t.beginTouchY);
                // Reject non-finite / non-positive factors: mesh untouched.
                if (!std::isfinite(f) || f <= 0.0f) return Status::ErrInvalidArg;
                t.accumFactor = ops::clampAccumScale(f);
                for (size_t i = 0; i < t.verts.size(); ++i) {
                    const ElemId v = t.verts[i];
                    if (obj.mesh.vertexAlive(v)) {
                        obj.mesh.vrt(v).pos =
                                t.pivot + (t.startPos[i] - t.pivot) * t.accumFactor;
                    }
                }
            } else { // axial scale (kind 1)
                const ops::AxisScreen as{map.axisDirX, map.axisDirY, map.pxPerWorld};
                const float f = ops::scaleAxisFromDrag(as, dx, dy);
                if (!std::isfinite(f) || f <= 0.0f) return Status::ErrInvalidArg;
                t.accumFactor = ops::clampAccumScale(t.accumFactor * f);
                const Vec3 axis = axisUnit(t.axis);
                for (size_t i = 0; i < t.verts.size(); ++i) {
                    const ElemId v = t.verts[i];
                    if (!obj.mesh.vertexAlive(v)) continue;
                    const Vec3 rel0 = t.startPos[i] - t.pivot;
                    const float along = dot(rel0, axis);
                    obj.mesh.vrt(v).pos =
                            t.pivot + rel0 + axis * (along * (t.accumFactor - 1.0f));
                }
            }
            break;
        }
        case Tool::None:
            return Status::ErrInvalidArg;
    }
    s.bump();
    return Status::OK;
}

Status endTool(Scene& s, History& h, ToolSession& t) {
    if (!t.active) return Status::ErrInvalidArg;
    if (t.objIndex < 0 || t.objIndex >= static_cast<int>(s.objects.size())) {
        t = ToolSession{};
        return Status::ErrNoActiveObject;
    }
    SceneObject& obj = s.objects[static_cast<size_t>(t.objIndex)];
    std::vector<std::pair<ElemId, Vec3>> before, after;
    before.reserve(t.verts.size());
    after.reserve(t.verts.size());
    for (size_t i = 0; i < t.verts.size(); ++i) {
        const ElemId v = t.verts[i];
        if (!obj.mesh.vertexAlive(v)) continue;
        before.emplace_back(v, t.startPos[i]);
        after.emplace_back(v, obj.mesh.vrt(v).pos);
    }
    // Defensive: a committed transform must never write non-finite geometry.
    for (const auto& [id, pos] : after) {
        if (!ops::isFiniteVec(pos.x, pos.y, pos.z)) {
            for (size_t i = 0; i < t.verts.size(); ++i) {
                if (obj.mesh.vertexAlive(t.verts[i])) {
                    obj.mesh.vrt(t.verts[i]).pos = t.startPos[i];
                }
            }
            h.rollbackReservation();
            s.bump();
            t = ToolSession{};
            return Status::ErrInternal;
        }
    }
    const bool moved = [&] {
        for (size_t i = 0; i < before.size(); ++i) {
            const Vec3 d = before[i].second - after[i].second;
            if (std::fabs(d.x) + std::fabs(d.y) + std::fabs(d.z) > 1e-7f) return true;
        }
        return false;
    }();
    if (moved) {
        if (!h.pushVerticesDelta(static_cast<uint32_t>(t.objIndex), std::move(before),
                                 std::move(after))) {
            // Reservation was pending; restore the start positions and cancel.
            for (size_t i = 0; i < t.verts.size(); ++i) {
                if (obj.mesh.vertexAlive(t.verts[i])) {
                    obj.mesh.vrt(t.verts[i]).pos = t.startPos[i];
                }
            }
            h.rollbackReservation();
            s.bump();
            t = ToolSession{};
            return Status::ErrInternal;
        }
    } else {
        h.rollbackReservation();
    }
    s.bump();
    t = ToolSession{};
    return Status::OK;
}

void cancelTool(Scene& s, History& h, ToolSession& t) {
    if (!t.active) return;
    if (t.objIndex >= 0 && t.objIndex < static_cast<int>(s.objects.size())) {
        SceneObject& obj = s.objects[static_cast<size_t>(t.objIndex)];
        for (size_t i = 0; i < t.verts.size(); ++i) {
            if (obj.mesh.vertexAlive(t.verts[i])) {
                obj.mesh.vrt(t.verts[i]).pos = t.startPos[i];
            }
        }
    }
    h.rollbackReservation();
    s.bump();
    t = ToolSession{};
}

// ------------------------------------------------- direct step transforms --

Status transformDirect(Scene& s, History& h, Tool tool, int axis, float amount) {
    SceneObject* obj = s.activeObject();
    if (!obj) return Status::ErrNoActiveObject;
    if (tool == Tool::None) return Status::ErrInvalidArg;
    if (axis < 0 || axis > 3) return Status::ErrInvalidArg;
    // axis==3 means "uniform" — only Scale accepts it.
    if (tool != Tool::Scale && axis == 3) return Status::ErrInvalidArg;
    // Non-finite amounts are rejected BEFORE anything changes (same policy
    // as the gizmo path — stability correction).
    if (!std::isfinite(amount)) return Status::ErrInvalidArg;

    // Target: EDIT mode valid selection, otherwise the whole active object.
    std::set<ElemId> verts;
    if (s.editMode) {
        for (const ElemId v : obj->sel.verts) verts.insert(v);
        for (const ElemId e : obj->sel.edges) {
            if (!obj->mesh.edgeAlive(e)) continue;
            const ElemId he = obj->mesh.edg(e).he;
            verts.insert(obj->mesh.heg(he).origin);
            verts.insert(obj->mesh.heg(obj->mesh.heg(he).twin).origin);
        }
        for (const ElemId f : obj->sel.faces) {
            for (const ElemId v : obj->mesh.faceVertices(f)) verts.insert(v);
        }
    }
    if (verts.empty()) {
        for (const auto& v : obj->mesh.vertices()) {
            if (v.alive) verts.insert(v.id);
        }
    }
    if (verts.empty()) return Status::ErrNoSelection;

    const std::vector<ElemId> targets(verts.begin(), verts.end());
    Vec3 pivot{};
    for (const ElemId v : targets) pivot = pivot + obj->mesh.vrt(v).pos;
    pivot = pivot * (1.0f / static_cast<float>(targets.size()));

    // Capture BEFORE, reserve history, mutate, validate, commit.
    std::vector<std::pair<ElemId, Vec3>> before;
    before.reserve(targets.size());
    for (const ElemId v : targets) before.emplace_back(v, obj->mesh.vrt(v).pos);
    const uint64_t cost = static_cast<uint64_t>(targets.size()) * 2ull * 16ull + 256;
    if (!h.reserve(cost)) return Status::ErrBudget;

    const Vec3 axisVec = axisUnit(axis < 3 ? axis : 0); // axis==3: unused (uniform)
    switch (tool) {
        case Tool::Move: {
            ops::applyMove(obj->mesh, targets, axisVec * amount);
            break;
        }
        case Tool::Rotate: {
            ops::applyRotation(obj->mesh, targets, pivot, axisVec, amount);
            break;
        }
        case Tool::Scale: {
            const float f = ops::clampAccumScale(amount);
            if (!std::isfinite(f) || f <= 0.0f) {
                h.rollbackReservation();
                return Status::ErrInvalidArg;
            }
            if (axis == 3) {
                ops::applyScale(obj->mesh, targets, pivot, f);
            } else {
                ops::applyAxisScale(obj->mesh, targets, pivot, axisVec, f);
            }
            break;
        }
        case Tool::None:
            h.rollbackReservation();
            return Status::ErrInvalidArg;
    }

    // Post-validation: a committed transform must never write non-finite
    // geometry (same contract as endTool).
    std::vector<std::pair<ElemId, Vec3>> after;
    after.reserve(targets.size());
    for (const ElemId v : targets) after.emplace_back(v, obj->mesh.vrt(v).pos);
    for (const auto& [id, pos] : after) {
        if (!ops::isFiniteVec(pos.x, pos.y, pos.z)) {
            for (const auto& [vid, orig] : before) {
                if (obj->mesh.vertexAlive(vid)) obj->mesh.vrt(vid).pos = orig;
            }
            h.rollbackReservation();
            s.bump();
            return Status::ErrInternal;
        }
    }
    const bool moved = [&] {
        for (size_t i = 0; i < before.size(); ++i) {
            const Vec3 d = before[i].second - after[i].second;
            if (std::fabs(d.x) + std::fabs(d.y) + std::fabs(d.z) > 1e-7f) return true;
        }
        return false;
    }();
    if (!moved) {
        h.rollbackReservation();
        return Status::OK;
    }
    // Copies (not moves): 'before' must stay valid for the failure restore.
    if (!h.pushVerticesDelta(static_cast<uint32_t>(s.active), before, after)) {
        for (const auto& [vid, orig] : before) {
            if (obj->mesh.vertexAlive(vid)) obj->mesh.vrt(vid).pos = orig;
        }
        h.rollbackReservation();
        s.bump();
        return Status::ErrInternal;
    }
    s.bump();
    return Status::OK;
}

// ----------------------------------------------------------------- brush --
//
// Granularity (Fase 6 contract): ONE history entry per gesture. The session
// accumulates the union of affected vertices; start states are captured on
// FIRST touch, so undo restores exactly the pre-gesture data.

namespace {

// Inserts `v` into the sorted-unique `touched` vector and, when new,
// captures its current position/color into the parallel start arrays.
// Returns the insertion index.
size_t touchVertex(BrushSession& b, const HalfEdgeMesh& m, ElemId v) {
    auto it = std::lower_bound(b.touched.begin(), b.touched.end(), v);
    if (it != b.touched.end() && *it == v) {
        return static_cast<size_t>(it - b.touched.begin());
    }
    const size_t idx = static_cast<size_t>(it - b.touched.begin());
    b.touched.insert(it, v);
    b.startPos.insert(b.startPos.begin() + static_cast<long>(idx),
                      m.vrt(v).pos);
    b.startColor.insert(b.startColor.begin() + static_cast<long>(idx),
                        m.vrt(v).color);
    return idx;
}

// One brush application at `hitLocal`: collect -> record start states ->
// apply -> mark change. Returns false only on a rejected application
// (invalid hit data / non-finite result, mesh untouched in that case).
bool applyBrushAt(Scene& s, BrushSession& b, const Vec3& hitLocal,
                  const Vec3& hitNormal) {
    if (b.objIndex < 0 || b.objIndex >= static_cast<int>(s.objects.size())) {
        return false;
    }
    HalfEdgeMesh& m = s.objects[static_cast<size_t>(b.objIndex)].mesh;
    if (!ops::isFiniteVec(hitLocal.x, hitLocal.y, hitLocal.z)) return false;

    std::vector<ElemId> affected;
    brush::collectAffected(m, hitLocal, b.settings.radius, affected);
    if (affected.empty()) return true; // nothing in range: valid no-op

    // Record start states BEFORE this application touches anything.
    for (const ElemId v : affected) touchVertex(b, m, v);

    const bool ok = (b.tool == BrushTool::Sculpt)
                        ? brush::sculptApply(m, affected, hitLocal, hitNormal,
                                             b.settings)
                        : brush::paintApply(m, affected, hitLocal, b.settings);
    if (!ok) return false;
    b.anyChange = true;
    return true;
}

} // namespace

Status beginBrushStroke(Scene& s, History& h, BrushSession& b, BrushTool tool,
                        int objIndex, const Vec3& hitLocal,
                        const Vec3& hitNormal,
                        const brush::BrushSettings& settings) {
    if (b.active) cancelBrushStroke(s, h, b); // defensive: one stroke at a time
    if (tool != BrushTool::Sculpt && tool != BrushTool::Paint) {
        return Status::ErrInvalidArg;
    }
    if (objIndex < 0 || objIndex >= static_cast<int>(s.objects.size())) {
        return Status::ErrNoActiveObject;
    }
    if (!brush::settingsValid(settings)) return Status::ErrInvalidArg;
    if (!ops::isFiniteVec(hitLocal.x, hitLocal.y, hitLocal.z)) {
        return Status::ErrInvalidArg;
    }
    if (!ops::isFiniteVec(hitNormal.x, hitNormal.y, hitNormal.z)) {
        return Status::ErrInvalidArg;
    }
    SceneObject& obj = s.objects[static_cast<size_t>(objIndex)];
    if (obj.mesh.empty()) return Status::ErrNoActiveObject;

    // History reservation BEFORE any mutation (condition 3). Upper bound for
    // the WHOLE stroke: every alive vertex could be touched (16 bytes per
    // delta pair, before+after) + slack. Rejected up front if it cannot fit.
    const uint64_t bound =
            static_cast<uint64_t>(obj.mesh.aliveVertexCount()) * 32ull + 256ull;
    if (!h.reserve(bound)) return Status::ErrBudget;

    BrushSession session;
    session.active = true;
    session.tool = tool;
    session.objIndex = objIndex;
    session.settings = settings;

    b = std::move(session);
    if (!applyBrushAt(s, b, hitLocal, hitNormal)) {
        // First dab rejected: clean cancellation, budget released.
        cancelBrushStroke(s, h, b);
        return Status::ErrInvalidArg;
    }
    s.bump(); // renderer resync (first dab is visible immediately)
    return Status::OK;
}

Status moveBrushStroke(Scene& s, BrushSession& b, const Vec3& hitLocal,
                       const Vec3& hitNormal, float radius, float strength) {
    if (!b.active) return Status::ErrInvalidArg;
    if (b.objIndex < 0 || b.objIndex >= static_cast<int>(s.objects.size())) {
        return Status::ErrNoActiveObject;
    }
    if (!ops::isFiniteVec(hitLocal.x, hitLocal.y, hitLocal.z)) {
        return Status::ErrInvalidArg;
    }
    if (!ops::isFiniteVec(hitNormal.x, hitNormal.y, hitNormal.z)) {
        return Status::ErrInvalidArg;
    }
    // Live parameters (real inputs, clamped through the same sanitizer).
    brush::BrushSettings next = b.settings;
    next.radius = radius;
    next.strength = strength;
    if (!brush::settingsValid(next)) return Status::ErrInvalidArg;
    b.settings.radius = brush::sanitized(next).radius;
    b.settings.strength = brush::sanitized(next).strength;

    if (!applyBrushAt(s, b, hitLocal, hitNormal)) return Status::ErrInternal;
    s.bump();
    return Status::OK;
}

Status endBrushStroke(Scene& s, History& h, BrushSession& b) {
    if (!b.active) return Status::ErrInvalidArg;
    const bool hadObject =
            b.objIndex >= 0 && b.objIndex < static_cast<int>(s.objects.size());
    HalfEdgeMesh* m = hadObject
                          ? &s.objects[static_cast<size_t>(b.objIndex)].mesh
                          : nullptr;

    // Commit path: build the exact before/after delta of the WHOLE gesture
    // from the accumulated union (touched) and push ONE history entry.
    // `touched` is sorted-unique and iterated in order, so index i IS the
    // parallel index into startPos/startColor.
    bool ok = true;
    bool committed = false;
    if (m != nullptr && b.anyChange) {
        std::vector<std::pair<ElemId, Vec3>> before, after;
        before.reserve(b.touched.size());
        after.reserve(b.touched.size());
        for (size_t i = 0; i < b.touched.size(); ++i) {
            const ElemId v = b.touched[i];
            if (!m->vertexAlive(v)) continue;
            if (b.tool == BrushTool::Sculpt) {
                before.emplace_back(v, b.startPos[i]);
                after.emplace_back(v, m->vrt(v).pos);
            } else {
                before.emplace_back(v, b.startColor[i]);
                after.emplace_back(v, m->vrt(v).color);
            }
        }
        const bool changed = [&] {
            for (size_t i = 0; i < before.size(); ++i) {
                const Vec3 d = before[i].second - after[i].second;
                if (std::fabs(d.x) + std::fabs(d.y) + std::fabs(d.z) > 1e-7f) {
                    return true;
                }
            }
            return false;
        }();
        if (changed) {
            committed = (b.tool == BrushTool::Sculpt)
                            ? h.pushVerticesDelta(
                                  static_cast<uint32_t>(b.objIndex),
                                  std::move(before), std::move(after))
                            : h.pushColorDelta(
                                  static_cast<uint32_t>(b.objIndex),
                                  std::move(before), std::move(after));
            ok = committed;
        } else {
            h.rollbackReservation();
            ok = true;
        }
    } else {
        h.rollbackReservation();
        ok = true;
    }
    if (!ok && m != nullptr) {
        // Push failed after mutation: restore the start state (same contract
        // as endTool) — the reservation is released either way.
        for (size_t i = 0; i < b.touched.size(); ++i) {
            const ElemId v = b.touched[i];
            if (!m->vertexAlive(v)) continue;
            if (b.tool == BrushTool::Sculpt) m->vrt(v).pos = b.startPos[i];
            else m->vrt(v).color = b.startColor[i];
        }
        h.rollbackReservation();
        s.bump();
    }
    b = BrushSession{};
    return ok ? Status::OK : Status::ErrInternal;
}

void cancelBrushStroke(Scene& s, History& h, BrushSession& b) {
    if (!b.active) return;
    if (b.objIndex >= 0 && b.objIndex < static_cast<int>(s.objects.size())) {
        HalfEdgeMesh& m = s.objects[static_cast<size_t>(b.objIndex)].mesh;
        for (size_t i = 0; i < b.touched.size(); ++i) {
            const ElemId v = b.touched[i];
            if (!m.vertexAlive(v)) continue;
            m.vrt(v).pos = b.startPos[i];
            m.vrt(v).color = b.startColor[i];
        }
    }
    h.rollbackReservation();
    s.bump();
    b = BrushSession{};
}

// -------------------------------------------------------- history / scene --

Status undo(Scene& s, History& h) {
    if (!h.undo(s)) return Status::ErrNoSelection;
    return Status::OK;
}
Status redo(Scene& s, History& h) {
    if (!h.redo(s)) return Status::ErrNoSelection;
    return Status::OK;
}
Status newScene(Scene& s, History& h) {
    s.clearScene();
    h.clear();
    return Status::OK;
}

} // namespace blone::sceneops
