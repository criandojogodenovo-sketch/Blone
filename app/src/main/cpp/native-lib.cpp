// JNI bridge between the Kotlin/Compose UI and the C++ core/renderer.
//
// Threading contract:
//  - surfaceCreated/surfaceChanged/drawFrame run on the GLSurfaceView render
//    thread, which owns the current OpenGL ES 3.0 context.
//  - pickAndSelect runs via GLSurfaceView.queueEvent (also the GL thread).
//  - gizmoHitTest/beginGizmoDrag/updateGizmoDrag run on the UI thread (cheap,
//    CPU-only) — every entry point takes the same mutex, so no call ever
//    observes a half-applied operation.
#include <jni.h>

#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "core/Log.h"
#include "core/history/History.h"
#include "core/mesh/Primitives.h"
#include "core/operations/Operations.h"
#include "core/operations/TransformMath.h"
#include "core/project/Project.h"
#include "core/render/RenderData.h"
#include "core/scene/Scene.h"
#include "core/scene/SceneOps.h"
#include "renderer/Renderer.h"

namespace {

blone::Renderer gRenderer;
blone::Scene gScene;
blone::History gHistory;
blone::sceneops::ToolSession gTool;
blone::GizmoTool gGizmoTool = blone::GizmoTool::Move;
std::mutex gMutex;

blone::OrbitCamera cameraFrom(jfloat azimuthDeg, jfloat elevationDeg,
                              jfloat distance, jfloat tx, jfloat ty, jfloat tz) {
    blone::OrbitCamera c;
    c.azimuthDeg = azimuthDeg;
    c.elevationDeg = elevationDeg > 85.0f ? 85.0f : (elevationDeg < -85.0f ? -85.0f : elevationDeg);
    c.distance = distance < 2.5f ? 2.5f : (distance > 40.0f ? 40.0f : distance);
    c.target = blone::Vec3{tx, ty, tz};
    return c;
}

std::vector<jint> selectionCounts() {
    const blone::SceneObject* obj = gScene.activeObject();
    if (!obj) return {0, 0, 0};
    return {static_cast<jint>(obj->sel.verts.size()),
            static_cast<jint>(obj->sel.edges.size()),
            static_cast<jint>(obj->sel.faces.size())};
}

jintArray toJIntArray(JNIEnv* env, const std::vector<jint>& v) {
    jintArray arr = env->NewIntArray(static_cast<jsize>(v.size()));
    if (!arr) return nullptr;
    env->SetIntArrayRegion(arr, 0, static_cast<jsize>(v.size()), v.data());
    return arr;
}

// Selection pivot in WORLD space — identical rule to the renderer's
// gizmoPivot (centroid of the selection; whole object when empty).
blone::Vec3 gizmoPivotForJni(const blone::Scene& scene, int& objIndexOut) {
    objIndexOut = scene.active;
    const blone::SceneObject* obj = scene.activeObject();
    if (!obj || obj->mesh.empty()) return blone::Vec3{};
    blone::Vec3 acc{};
    size_t count = 0;
    for (const blone::ElemId v : obj->sel.verts) {
        if (!obj->mesh.vertexAlive(v)) continue;
        acc = acc + obj->mesh.vrt(v).pos;
        ++count;
    }
    for (const blone::ElemId e : obj->sel.edges) {
        if (!obj->mesh.edgeAlive(e)) continue;
        const blone::ElemId h = obj->mesh.edg(e).he;
        acc = acc + obj->mesh.vrt(obj->mesh.heg(h).origin).pos;
        acc = acc + obj->mesh.vrt(obj->mesh.heg(obj->mesh.heg(h).twin).origin).pos;
        count += 2;
    }
    for (const blone::ElemId f : obj->sel.faces) {
        if (!obj->mesh.faceAlive(f)) continue;
        for (const blone::ElemId v : obj->mesh.faceVertices(f)) {
            acc = acc + obj->mesh.vrt(v).pos;
            ++count;
        }
    }
    if (count == 0) {
        for (const auto& v : obj->mesh.vertices()) {
            if (!v.alive) continue;
            acc = acc + v.pos;
            ++count;
        }
    }
    if (count == 0) return blone::Vec3{};
    const blone::Vec3 local = acc * (1.0f / static_cast<float>(count));
    return local + obj->position;
}

// Pixel-space mapping of a world axis at the pivot for gizmo drags: screen
// direction (y down), apparent pixels-per-world-unit and the ring sign that
// makes the surface point under the finger follow the finger.
blone::sceneops::GizmoMapping gizmoMapping(const blone::OrbitCamera& cam,
                                           int axis, float viewportW,
                                           float viewportH) {
    blone::sceneops::GizmoMapping map;
    const blone::Vec3 axes[3] = {blone::Vec3{1, 0, 0}, blone::Vec3{0, 1, 0},
                                 blone::Vec3{0, 0, 1}};
    const blone::Vec3 axisDir = (axis >= 0 && axis <= 2) ? axes[axis] : axes[0];

    // Selection pivot in world space (same rule as the renderer's gizmo).
    int objIndex = -1;
    blone::Vec3 pivot = gizmoPivotForJni(gScene, objIndex);
    const float vpAxisLen = 1.0f; // project a 1-unit step of the axis

    const blone::Mat4 proj = blone::Mat4::perspective(
            cam.fovYDeg,
            viewportH > 0.0f ? viewportW / viewportH : 1.0f, 0.1f, 200.0f);
    const blone::Mat4 view = cam.viewMatrix();
    const blone::Mat4 vp = proj * view;

    auto projPx = [&](const blone::Vec3& w, float& sx, float& sy) -> bool {
        const float cx = vp.m[0] * w.x + vp.m[4] * w.y + vp.m[8] * w.z + vp.m[12];
        const float cy = vp.m[1] * w.x + vp.m[5] * w.y + vp.m[9] * w.z + vp.m[13];
        const float cw = vp.m[3] * w.x + vp.m[7] * w.y + vp.m[11] * w.z + vp.m[15];
        if (cw <= 1e-6f) return false;
        sx = ((cx / cw) * 0.5f + 0.5f) * viewportW;
        sy = (1.0f - ((cy / cw) * 0.5f + 0.5f)) * viewportH;
        return true;
    };

    float px = 0.0f, py = 0.0f, qx = 0.0f, qy = 0.0f;
    if (!projPx(pivot, px, py) || !projPx(pivot + axisDir * vpAxisLen, qx, qy)) {
        return map; // degenerate: identity mapping (never mutates wrongly)
    }
    float dx = qx - px, dy = qy - py;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > 1e-6f) {
        map.axisDirX = dx / len;
        map.axisDirY = dy / len;
        map.pxPerWorld = len / vpAxisLen;
    }
    map.pivotScreenX = px;
    map.pivotScreenY = py;

    // Ring sign: the pixel-space sweep (y down) is positive for visually
    // CLOCKWISE moves. For a ring whose axis points TOWARDS the camera, a
    // positive right-hand rotation appears counter-clockwise, so the sign
    // must invert; away from the camera it matches. (Pinned by a host test:
    // the surface point under the finger follows the finger.)
    const blone::Vec3 toPivot = pivot - cam.eye();
    map.ringSign = (dot(axisDir, toPivot) > 0.0f) ? -1.0f : 1.0f;
    return map;
}


// Logs the mandatory cube validation (correção, secção 8): counts, winding
// and topological validation, distinguishing EDITABLE mesh vertices from the
// DUPLICATED vertices of the render buffer.
void logCubeValidation(const blone::SceneObject& obj,
                       const blone::renderdata::RenderData& rd) {
    const blone::HalfEdgeMesh& m = obj.mesh;
    if (m.aliveFaceCount() != 6) return; // only the plain cube is validated here

    // Outward winding: every face normal must point away from the mesh center.
    blone::Vec3 center{};
    size_t n = 0;
    for (const auto& v : m.vertices()) {
        if (!v.alive) continue;
        center = center + v.pos;
        ++n;
    }
    center = center * (1.0f / static_cast<float>(n));
    int outward = 0;
    for (const blone::ElemId f : m.aliveFaceIds()) {
        const blone::Vec3 c = m.faceCentroid(f) - center;
        if (dot(m.faceNormal(f), c) > 0.0f) ++outward;
    }
    BLONE_LOGI(
            "Cubo validado: vertices topologicos=%zu faces=%zu triangulos_render=%zu "
            "| vertices duplicados no buffer (flat shading)=%zu arestas=%zu "
            "| winding exterior=%d/6 | topologia=%s",
            m.aliveVertexCount(), m.aliveFaceCount(), rd.triangleCount,
            rd.fillVertexCount, m.aliveEdgeCount(), outward,
            m.validate() ? "VALIDA" : "INVALIDA");
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_surfaceCreated(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    gRenderer.init();
}

extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_surfaceChanged(JNIEnv* /*env*/, jobject /*thiz*/,
                                               jint width, jint height) {
    std::lock_guard<std::mutex> lock(gMutex);
    gRenderer.resize(width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_drawFrame(JNIEnv* /*env*/, jobject /*thiz*/,
                                          jfloat azimuthDeg, jfloat elevationDeg,
                                          jfloat distance, jfloat tx, jfloat ty,
                                          jfloat tz, jfloat pointRadiusPx) {
    std::lock_guard<std::mutex> lock(gMutex);
    const blone::OrbitCamera cam =
            cameraFrom(azimuthDeg, elevationDeg, distance, tx, ty, tz);
    gRenderer.render(cam, gScene, pointRadiusPx, gGizmoTool);
}

// Tap picking + selection. Returns [kind, objIndex, elemId, nV, nE, nF].
extern "C" JNIEXPORT jintArray JNICALL
Java_com_blone_app_gl_NativeLib_pickAndSelect(JNIEnv* env, jobject /*thiz*/,
                                              jfloat xPx, jfloat yPx,
                                              jfloat snapRadiusPx) {
    std::lock_guard<std::mutex> lock(gMutex);
    blone::Renderer::PickHit hit = gRenderer.pickAtPixel(xPx, yPx, gScene, snapRadiusPx);
    std::vector<jint> out{0, -1, 0, 0, 0, 0};
    if (hit.hit) {
        blone::ops::Status st = blone::sceneops::selectFromPick(
                gScene, hit.kind, hit.objectIndex, hit.elemId);
        out = {static_cast<jint>(hit.kind), hit.objectIndex,
               static_cast<jint>(hit.elemId), 0, 0, 0};
        if (st != blone::ops::Status::OK) {
            out[0] = -static_cast<jint>(st); // negative kind = selection error
        }
        const std::vector<jint> counts = selectionCounts();
        out[3] = counts[0];
        out[4] = counts[1];
        out[5] = counts[2];
    }
    return toJIntArray(env, out);
}

// Gizmo tool shown in edit mode: 1 move, 2 rotate, 3 scale.
extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_setGizmoTool(JNIEnv* /*env*/, jobject /*thiz*/, jint tool) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (tool >= 1 && tool <= 3) {
        gGizmoTool = static_cast<blone::GizmoTool>(tool);
    }
}

// CPU hit-test against the gizmo handles. Returns the handle id
// (0 none, 1..3 axis tips, 4 uniform-scale center, 11..13 rotation rings).
extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_gizmoHitTest(JNIEnv* /*env*/, jobject /*thiz*/,
                                             jfloat xPx, jfloat yPx) {
    std::lock_guard<std::mutex> lock(gMutex);
    const blone::Renderer::GizmoHit hit = gRenderer.gizmoHitAt(xPx, yPx, gScene, gGizmoTool);
    return hit.hit ? hit.handle : 0;
}

// Starts a gizmo drag. handle: 1..3 axis tip, 4 center, 11..13 ring.
extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_beginGizmoDrag(JNIEnv* /*env*/, jobject /*thiz*/,
                                               jint handle, jfloat xPx, jfloat yPx,
                                               jfloat az, jfloat el, jfloat dist,
                                               jfloat tx, jfloat ty, jfloat tz,
                                               jfloat viewportW, jfloat viewportH) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (handle < 1 || handle > 13) return static_cast<jint>(blone::ops::Status::ErrInvalidArg);
    int tool = 0;
    int kind = 0;
    int axis = -1;
    if (handle >= 1 && handle <= 3) {
        tool = static_cast<int>(blone::GizmoTool::Move);
        kind = 1;
        axis = handle - 1;
    } else if (handle == 4) {
        tool = static_cast<int>(blone::GizmoTool::Scale);
        kind = 3;
        axis = 3;
    } else if (handle >= 11 && handle <= 13) {
        tool = static_cast<int>(blone::GizmoTool::Rotate);
        kind = 2;
        axis = handle - 11;
    }
    const blone::sceneops::Tool t = static_cast<blone::sceneops::Tool>(tool);
    if (static_cast<int>(gGizmoTool) != tool) {
        return static_cast<jint>(blone::ops::Status::ErrInvalidArg);
    }
    return static_cast<jint>(blone::sceneops::beginTool(
            gScene, gHistory, gTool, t, kind, axis, xPx, yPx));
}

// Continues a gizmo drag with ABSOLUTE touch pixels.
extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_updateGizmoDrag(JNIEnv* /*env*/, jobject /*thiz*/,
                                                jfloat xPx, jfloat yPx,
                                                jfloat az, jfloat el, jfloat dist,
                                                jfloat tx, jfloat ty, jfloat tz,
                                                jfloat viewportW, jfloat viewportH) {
    std::lock_guard<std::mutex> lock(gMutex);
    const blone::OrbitCamera cam = cameraFrom(az, el, dist, tx, ty, tz);
    const int axis = (gTool.axis >= 0 && gTool.axis <= 2) ? gTool.axis : 0;
    const blone::sceneops::GizmoMapping map =
            gizmoMapping(cam, axis, viewportW, viewportH);
    return static_cast<jint>(
            blone::sceneops::updateTool(gScene, gTool, map, xPx, yPx));
}

// Finishes a gizmo drag; commits the history entry when something moved.
extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_endGizmoDrag(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<jint>(blone::sceneops::endTool(gScene, gHistory, gTool));
}

// Cancels a gizmo drag (second finger landed — the camera takes over).
extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_cancelGizmoDrag(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    blone::sceneops::cancelTool(gScene, gHistory, gTool);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_createPrimitive(JNIEnv* /*env*/, jobject /*thiz*/,
                                                jint type, jint segsU, jint segsV) {
    std::lock_guard<std::mutex> lock(gMutex);
    blone::PrimType t = static_cast<blone::PrimType>(type);
    const int index =
            blone::sceneops::createPrimitive(gScene, gHistory, t, segsU, segsV);
    if (index >= 0 && t == blone::PrimType::Cube) {
        blone::renderdata::RenderData rd;
        blone::renderdata::buildObject(gScene.objects[static_cast<size_t>(index)], rd);
        logCubeValidation(gScene.objects[static_cast<size_t>(index)], rd);
    }
    return index;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_objectCount(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<jint>(gScene.objects.size());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_blone_app_gl_NativeLib_objectName(JNIEnv* env, jobject /*thiz*/, jint index) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (index < 0 || index >= static_cast<jint>(gScene.objects.size())) {
        return env->NewStringUTF("");
    }
    return env->NewStringUTF(gScene.objects[static_cast<size_t>(index)].name.c_str());
}

// Returns [vertices, edges, faces, active(0/1)] for the object, or null.
extern "C" JNIEXPORT jintArray JNICALL
Java_com_blone_app_gl_NativeLib_objectInfo(JNIEnv* env, jobject /*thiz*/, jint index) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (index < 0 || index >= static_cast<jint>(gScene.objects.size())) return nullptr;
    const blone::SceneObject& obj = gScene.objects[static_cast<size_t>(index)];
    std::vector<jint> out{
            static_cast<jint>(obj.mesh.aliveVertexCount()),
            static_cast<jint>(obj.mesh.aliveEdgeCount()),
            static_cast<jint>(obj.mesh.aliveFaceCount()),
            (index == gScene.active) ? 1 : 0};
    return toJIntArray(env, out);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_setActiveObject(JNIEnv* /*env*/, jobject /*thiz*/,
                                                jint index) {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<jint>(
            blone::sceneops::setActiveObject(gScene, index));
}

extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_setEditMode(JNIEnv* /*env*/, jobject /*thiz*/,
                                            jboolean edit) {
    std::lock_guard<std::mutex> lock(gMutex);
    blone::sceneops::setEditMode(gScene, edit == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_setSelectionMode(JNIEnv* /*env*/, jobject /*thiz*/,
                                                 jint mode) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (mode >= 0 && mode <= 2) {
        blone::sceneops::setSelectionMode(gScene, static_cast<blone::SelMode>(mode));
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_setMultiSelect(JNIEnv* /*env*/, jobject /*thiz*/,
                                               jboolean on) {
    std::lock_guard<std::mutex> lock(gMutex);
    blone::sceneops::setMultiSelect(gScene, on == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_clearSelection(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    blone::sceneops::clearSelection(gScene);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_extrude(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<jint>(blone::sceneops::extrude(gScene, gHistory));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_subdivide(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<jint>(blone::sceneops::subdivide(gScene, gHistory));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_deleteSelection(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<jint>(blone::sceneops::deleteSelection(gScene, gHistory));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_deleteActiveObject(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<jint>(blone::sceneops::deleteActiveObject(gScene, gHistory));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_loopCut(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<jint>(blone::sceneops::loopCut(gScene, gHistory));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_blone_app_gl_NativeLib_canUndo(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return gHistory.undoDepth() > 0 ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_blone_app_gl_NativeLib_canRedo(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return gHistory.redoDepth() > 0 ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_blone_app_gl_NativeLib_historyMemBytes(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<jlong>(gHistory.usedBytes());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_undo(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<jint>(blone::sceneops::undo(gScene, gHistory));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_redo(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<jint>(blone::sceneops::redo(gScene, gHistory));
}

// Serializes the current scene; returns null on failure.
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_blone_app_gl_NativeLib_saveProject(JNIEnv* env, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    std::vector<uint8_t> bytes = blone::project::save(gScene);
    if (bytes.empty()) return nullptr;
    jbyteArray arr = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (!arr) return nullptr;
    env->SetByteArrayRegion(arr, 0, static_cast<jsize>(bytes.size()),
                            reinterpret_cast<const jbyte*>(bytes.data()));
    return arr;
}

// Validates and loads; the current scene is only replaced on success.
extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_openProject(JNIEnv* env, jobject /*thiz*/,
                                            jbyteArray bytes) {
    if (!bytes) return blone::project::ErrSize;
    const jsize len = env->GetArrayLength(bytes);
    std::vector<uint8_t> data(static_cast<size_t>(len));
    env->GetByteArrayRegion(bytes, 0, len, reinterpret_cast<jbyte*>(data.data()));
    std::lock_guard<std::mutex> lock(gMutex);
    blone::Scene loaded;
    const int rc = blone::project::load(data, loaded);
    if (rc != blone::project::Ok) return rc;
    gScene = std::move(loaded);
    gHistory.clear();
    blone::sceneops::setEditMode(gScene, gScene.editMode); // refresh version
    return blone::project::Ok;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_newScene(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<jint>(blone::sceneops::newScene(gScene, gHistory));
}
