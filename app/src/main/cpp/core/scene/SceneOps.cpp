#include "SceneOps.h"

#include "../Log.h"

#include <algorithm>

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

const Selection::Set& setFor(const SceneObject& obj, SelMode mode) {
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
    const uint64_t bound = obj->mesh.rawBytes() * kBoundExtrude + 4096;
    if (!h.reserve(bound)) return Status::ErrBudget;

    std::set<ElemId> caps;
    const Status st = ops::extrudeFaces(obj->mesh, faces, caps);
    if (st != Status::OK) {
        h.rollbackReservation();
        return st; // mesh untouched: ops:: rolled back internally
    }
    if (!h.pushMeshSwap(static_cast<uint32_t>(s.active), pre, obj->mesh.captureRaw())) {
        obj->mesh.restoreRaw(pre);
        h.rollbackReservation();
        return Status::ErrInternal;
    }
    obj->sel.faces = caps;
    obj->sel.verts.clear();
    obj->sel.edges.clear();
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
    const uint64_t bound = obj->mesh.rawBytes() * kBoundSubdivide + 4096;
    if (!h.reserve(bound)) return Status::ErrBudget;

    const Status st = ops::subdivideFaces(obj->mesh, faces);
    if (st != Status::OK) {
        h.rollbackReservation();
        return st;
    }
    if (!h.pushMeshSwap(static_cast<uint32_t>(s.active), pre, obj->mesh.captureRaw())) {
        obj->mesh.restoreRaw(pre);
        h.rollbackReservation();
        return Status::ErrInternal;
    }
    obj->sel.clear();
    s.bump();
    return Status::OK;
}

Status deleteSelection(Scene& s, History& h) {
    SceneObject* obj = s.activeObject();
    if (!obj) return Status::ErrNoActiveObject;
    const std::vector<ElemId> sel = toVector(setFor(*obj, s.selectionMode));
    if (sel.empty()) return Status::ErrNoSelection;

    const HalfEdgeMesh::Raw pre = obj->mesh.captureRaw();
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
    if (!h.pushMeshSwap(static_cast<uint32_t>(s.active), pre, obj->mesh.captureRaw())) {
        obj->mesh.restoreRaw(pre);
        h.rollbackReservation();
        return Status::ErrInternal;
    }
    obj->sel.prune(obj->mesh);
    s.bump();
    return Status::OK;
}

Status loopCut(Scene& s, History& h) {
    SceneObject* obj = s.activeObject();
    if (!obj) return Status::ErrNoActiveObject;
    const std::vector<ElemId> edges = toVector(obj->sel.edges);
    if (edges.empty()) return Status::ErrNoSelection;

    const HalfEdgeMesh::Raw pre = obj->mesh.captureRaw();
    const uint64_t bound = obj->mesh.rawBytes() * kBoundLoopCut + 4096;
    if (!h.reserve(bound)) return Status::ErrBudget;

    ops::LoopCutReport rep;
    const Status st = ops::loopCut(obj->mesh, edges.front(), rep);
    if (st != Status::OK) {
        h.rollbackReservation();
        return st;
    }
    if (!h.pushMeshSwap(static_cast<uint32_t>(s.active), pre, obj->mesh.captureRaw())) {
        obj->mesh.restoreRaw(pre);
        h.rollbackReservation();
        return Status::ErrInternal;
    }
    obj->sel.clear();
    s.bump();
    return Status::OK;
}

// -------------------------------------------------------- transform tools --

Status beginTool(Scene& s, History& h, ToolSession& t, Tool tool, const DragParams& p) {
    (void)h;
    if (t.active) cancelTool(s, h, t);
    SceneObject* obj = s.activeObject();
    if (!obj) return Status::ErrNoActiveObject;
    if (!s.editMode) return Status::ErrInvalidArg;

    // Deduplicated vertex set from the current selection.
    std::set<ElemId> verts;
    for (const ElemId v : obj->sel.verts) verts.insert(v);
    for (const ElemId e : obj->sel.edges) {
        if (!obj->mesh.edgeAlive(e)) continue;
        const ElemId h = obj->mesh.edg(e).he;
        verts.insert(obj->mesh.heg(h).origin);
        verts.insert(obj->mesh.heg(obj->mesh.heg(h).twin).origin);
    }
    for (const ElemId f : obj->sel.faces) {
        for (const ElemId v : obj->mesh.faceVertices(f)) verts.insert(v);
    }
    if (verts.empty()) return Status::ErrNoSelection;

    ToolSession session;
    session.active = true;
    session.tool = tool;
    session.objIndex = s.active;
    session.verts.assign(verts.begin(), verts.end());
    session.params = p;
    session.accumDx = 0.0f;
    session.accumDy = 0.0f;
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

Status updateTool(Scene& s, ToolSession& t, const DragParams& p, float dxPx, float dyPx) {
    if (!t.active) return Status::ErrInvalidArg;
    if (t.objIndex < 0 || t.objIndex >= static_cast<int>(s.objects.size())) {
        return Status::ErrNoActiveObject;
    }
    SceneObject& obj = s.objects[static_cast<size_t>(t.objIndex)];
    t.accumDx += dxPx;
    t.accumDy += dyPx;
    t.params = p; // camera may move between events; deltas are cumulative

    const Vec3 delta = ops::moveWorldDelta(t.params, t.accumDx, t.accumDy);
    switch (t.tool) {
        case Tool::Move: {
            for (size_t i = 0; i < t.verts.size(); ++i) {
                const ElemId v = t.verts[i];
                if (obj.mesh.vertexAlive(v)) obj.mesh.vrt(v).pos = t.startPos[i] + delta;
            }
            break;
        }
        case Tool::Rotate: {
            const ops::DragRotation r = ops::rotationFromDrag(t.params, t.accumDx, t.accumDy);
            for (size_t i = 0; i < t.verts.size(); ++i) {
                const ElemId v = t.verts[i];
                if (!obj.mesh.vertexAlive(v)) continue;
                Vec3 rel = t.startPos[i] - t.pivot;
                rel = Mat4::rotationAxisDeg(r.axisRight, r.degRight).transformPoint(rel);
                rel = Mat4::rotationAxisDeg(r.axisY, r.degY).transformPoint(rel);
                obj.mesh.vrt(v).pos = t.pivot + rel;
            }
            break;
        }
        case Tool::Scale: {
            const float f = ops::scaleFromDrag(t.accumDy);
            for (size_t i = 0; i < t.verts.size(); ++i) {
                const ElemId v = t.verts[i];
                if (obj.mesh.vertexAlive(v)) {
                    obj.mesh.vrt(v).pos = t.pivot + (t.startPos[i] - t.pivot) * f;
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
