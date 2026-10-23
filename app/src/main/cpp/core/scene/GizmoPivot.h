#pragma once
// Shared gizmo pivot rule — used by BOTH the renderer (drawing/hit-test) and
// the JNI gizmo mapping, so the two can never diverge (audit finding #7:
// duplicated rules in Renderer.cpp and native-lib.cpp).
//
// Rule:
//   OBJECT mode: centroid of the WHOLE active object (local vertices mean +
//                object position). The gizmo transforms the entire object.
//   EDIT mode:   centroid of the ALIVE selection; the whole object when the
//                selection is empty (robustness fallback — the renderer still
//                gates gizmo drawing on hasValidSelection in edit mode).
// Returns false when there is no usable pivot (no active object or empty
// mesh) — callers must draw NO gizmo and accept NO hit in that case.
//
// F4-LIMITS (P3): READ-ONLY active objects (geometry in the shared
// RenderMesh, the HalfEdgeMesh intentionally empty) use the SAME whole-
// object rule with the RenderMesh's cached centroid as the source — the
// gate itself (which object gets a gizmo, and when) is UNCHANGED. Without
// this, the object gizmo could not exist in read mode (missão: "gizmo de
// objeto funciona em leitura").

#include "Scene.h"

namespace blone::gizmopivot {

inline bool pivotFor(const Scene& s, bool editMode, Vec3& out, int& objIndexOut) {
    objIndexOut = s.active;
    const SceneObject* obj = s.activeObject();
    if (!obj) return false;

    // F4-LIMITS (P3): read-only object — the same whole-object rule, the
    // centroid comes from the cached RenderMesh centroid (O(1); the pivot
    // must never walk half a million vertices per frame). Only meaningful
    // in OBJECT mode: edit mode has no alive elements on a read object, so
    // hasValidSelection is false and the callers never reach here.
    if (!obj->editable) {
        if (editMode) return false; // no selection can exist on a read object
        if (!obj->readMesh || obj->readMesh->empty()) return false;
        out = obj->readMesh->centroid + obj->position;
        return true;
    }

    if (obj->mesh.empty()) return false;

    Vec3 acc{};
    size_t count = 0;
    if (editMode) {
        for (const ElemId v : obj->sel.verts) {
            if (!obj->mesh.vertexAlive(v)) continue;
            acc = acc + obj->mesh.vrt(v).pos;
            ++count;
        }
        for (const ElemId e : obj->sel.edges) {
            if (!obj->mesh.edgeAlive(e)) continue;
            const ElemId h = obj->mesh.edg(e).he;
            acc = acc + obj->mesh.vrt(obj->mesh.heg(h).origin).pos;
            acc = acc + obj->mesh.vrt(obj->mesh.heg(obj->mesh.heg(h).twin).origin).pos;
            count += 2;
        }
        for (const ElemId f : obj->sel.faces) {
            if (!obj->mesh.faceAlive(f)) continue;
            for (const ElemId v : obj->mesh.faceVertices(f)) {
                acc = acc + obj->mesh.vrt(v).pos;
                ++count;
            }
        }
    }
    if (count == 0) {
        // Whole object (object mode, or empty selection in edit mode).
        for (const auto& v : obj->mesh.vertices()) {
            if (!v.alive) continue;
            acc = acc + v.pos;
            ++count;
        }
    }
    if (count == 0) return false;
    const Vec3 local = acc * (1.0f / static_cast<float>(count));
    out = local + obj->position;
    return true;
}

} // namespace blone::gizmopivot
