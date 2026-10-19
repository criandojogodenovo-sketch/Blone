// JNI bridge between the Kotlin/Compose UI and the C++ core/renderer (Fase 2).
//
// Threading contract:
//  - surfaceCreated/surfaceChanged/drawFrame run on the GLSurfaceView render
//    thread, which owns the current OpenGL ES 3.0 context.
//  - pickAndSelect runs via GLSurfaceView.queueEvent (also the GL thread).
//  - every scene-mutating call runs on whatever thread Kotlin chooses (UI or
//    a coroutine worker); a single mutex serializes scene + renderer access,
//    so no call ever observes a half-applied operation.
#include <jni.h>

#include <mutex>
#include <string>
#include <vector>

#include "core/Log.h"
#include "core/history/History.h"
#include "core/mesh/Primitives.h"
#include "core/operations/Operations.h"
#include "core/operations/TransformMath.h"
#include "core/project/Project.h"
#include "core/scene/Scene.h"
#include "core/scene/SceneOps.h"
#include "renderer/Renderer.h"

namespace {

blone::Renderer gRenderer;
blone::Scene gScene;
blone::History gHistory;
blone::sceneops::ToolSession gTool;
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
    gRenderer.render(cam, gScene, pointRadiusPx);
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

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_createPrimitive(JNIEnv* /*env*/, jobject /*thiz*/,
                                                jint type, jint segsU, jint segsV) {
    std::lock_guard<std::mutex> lock(gMutex);
    blone::PrimType t = static_cast<blone::PrimType>(type);
    return blone::sceneops::createPrimitive(gScene, gHistory, t, segsU, segsV);
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

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_beginTool(JNIEnv* /*env*/, jobject /*thiz*/,
                                          jint tool, jfloat az, jfloat el,
                                          jfloat dist, jfloat fovY,
                                          jfloat viewportH) {
    std::lock_guard<std::mutex> lock(gMutex);
    blone::ops::DragParams p;
    p.azimuthDeg = az;
    p.elevationDeg = el;
    p.distance = dist;
    p.fovYDeg = fovY;
    p.viewportHeightPx = viewportH;
    return static_cast<jint>(blone::sceneops::beginTool(
            gScene, gHistory, gTool, static_cast<blone::sceneops::Tool>(tool), p));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_updateTool(JNIEnv* /*env*/, jobject /*thiz*/,
                                           jfloat dx, jfloat dy, jfloat az,
                                           jfloat el, jfloat dist, jfloat fovY,
                                           jfloat viewportH) {
    std::lock_guard<std::mutex> lock(gMutex);
    blone::ops::DragParams p;
    p.azimuthDeg = az;
    p.elevationDeg = el;
    p.distance = dist;
    p.fovYDeg = fovY;
    p.viewportHeightPx = viewportH;
    return static_cast<jint>(blone::sceneops::updateTool(gScene, gTool, p, dx, dy));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_endTool(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<jint>(blone::sceneops::endTool(gScene, gHistory, gTool));
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
