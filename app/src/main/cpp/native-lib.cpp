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

#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include "core/Log.h"
#include "core/history/History.h"
#include "core/io/GltfLoader.h"
#include "core/io/MeshBuilder.h"
#include "core/io/ObjExporter.h"
#include "core/io/ObjLoader.h"
#include "core/mesh/Primitives.h"
#include "core/operations/Operations.h"
#include "core/operations/TransformMath.h"
#include "core/project/Project.h"
#include "core/render/RenderData.h"
#include "core/scene/GizmoPivot.h"
#include "core/scene/Scene.h"
#include "core/scene/SceneOps.h"
#include "renderer/Renderer.h"

namespace {

blone::Renderer gRenderer;
blone::Scene gScene;
blone::History gHistory;
blone::sceneops::ToolSession gTool;
blone::sceneops::BrushSession gBrush; // Fase 3: um gesto = uma sessão
blone::GizmoTool gGizmoTool = blone::GizmoTool::Move;
std::mutex gMutex;

// ============ Fase 4: I/O assíncrono (OBJ/glTF) ============================
//
// Threading contract (Regra 3 — a GL e a UI nunca bloqueiam em disco):
//  - workers (std::async): parse + MeshBuilder::build — NUNCA tocam na cena
//    viva; empurram para a fila e escrevem o estado de I/O;
//  - drenagem: no ponto por-frame já existente (drawFrame, thread GL), sob
//    gMutex, SEMPRE através da fachada SceneOps (1 entrada ObjAdd => undo
//    remove o import);
//  - exportação: snapshot profundo sob gMutex (captureRaw/restoreRaw por
//    objeto); a worker escreve o ficheiro apenas sobre o snapshot.
struct PendingImport {
    std::string name;      // sanitized display name
    std::string sourcePath; // for IMPORT_FAIL reporting at drain time
    blone::HalfEdgeMesh mesh;
};
std::mutex gIoMutex;                      // guards the queue AND the futures
std::deque<PendingImport> gPendingImports;
std::vector<std::future<void>> gIoFutures;
std::mutex gIoStatusMutex;                // guards the atomic status slot
std::string gIoStatus;

// Atomic status slot (protocol: "" idle | "IMPORT_OK|<nome>" |
// "IMPORT_FAIL|<path>" | "EXPORT_OK|<cachepath>" | "EXPORT_FAIL|<path>").
// Written by workers/drain, read+consumed by Kotlin OUTSIDE the GL thread.
void setIoStatus(std::string s) {
    std::lock_guard<std::mutex> lock(gIoStatusMutex);
    gIoStatus = std::move(s);
}

std::string takeIoStatus() {
    std::lock_guard<std::mutex> lock(gIoStatusMutex);
    std::string s = std::move(gIoStatus);
    gIoStatus.clear();
    return s;
}

std::string peekIoStatus() {
    std::lock_guard<std::mutex> lock(gIoStatusMutex);
    return gIoStatus;
}

// std::async trap: destroying a future whose shared state never finished
// BLOCKS the destroying thread. Futures are therefore kept in this vector
// and pruned ONLY when finished — a pending worker simply stays listed for
// the next prune. wait_for(0) is used instead of is_ready(): libc++ (NDK)
// does not expose is_ready on std::future (libstdc++ extension).
// Called before every new launch.
void pruneFinishedFutures() {
    std::lock_guard<std::mutex> lock(gIoMutex);
    gIoFutures.erase(std::remove_if(gIoFutures.begin(), gIoFutures.end(),
                                    [](std::future<void>& f) {
                                        return !f.valid() ||
                                               f.wait_for(std::chrono::seconds(0)) ==
                                               std::future_status::ready;
                                    }),
                     gIoFutures.end());
}

// DRAIN — runs on the GL thread inside drawFrame, caller holds gMutex.
// Imports enter the scene ONLY here: via the SceneOps facade, so every
// import is one history entry (undo removes it), respects kMaxObjects and
// bumps the scene for renderer resync. Queue swap happens under gIoMutex;
// the (possibly slow) scene mutation runs under gMutex but does NO disk I/O.
void pruneSelectionsLocked(); // defined below (shared selection hygiene)
void drainPendingImportsLocked() {
    std::deque<PendingImport> pending;
    {
        std::lock_guard<std::mutex> lock(gIoMutex);
        pending.swap(gPendingImports);
    }
    while (!pending.empty()) {
        PendingImport item = std::move(pending.front());
        pending.pop_front();
        const std::string name = item.name;
        blone::SceneObject obj;
        obj.name = std::move(item.name);
        obj.mesh = std::move(item.mesh);
        const blone::ops::Status st =
                blone::sceneops::addImportedObject(gScene, gHistory,
                                                   std::move(obj));
        if (st == blone::ops::Status::OK) {
            pruneSelectionsLocked();
            // Stage (e): the object REALLY entered the scene (undo removes
            // it, kMaxObjects respected by the SceneOps facade).
            BLONE_IOI("(e) drenagem: '%s' entrou na cena via SceneOps "
                      "(ObjAdd, objeto ativo = %d)",
                      name.c_str(), gScene.active);
            setIoStatus("IMPORT_OK|" + name);
        } else {
            // kMaxObjects / budget / internal: the scene stays intact and
            // the failure is reported against the source path WITH the PT
            // reason (hotfix D1/R8 — nunca silencioso).
            const char* motivo = "falha ao adicionar à cena";
            switch (st) {
                case blone::ops::Status::ErrLimit:
                    motivo = "cena cheia: máximo de objetos atingido";
                    break;
                case blone::ops::Status::ErrBudget:
                    motivo = "limite de histórico esgotado";
                    break;
                case blone::ops::Status::ErrInvalidArg:
                    motivo = "malha vazia";
                    break;
                case blone::ops::Status::ErrInternal:
                    motivo = "erro interno ao adicionar o objeto";
                    break;
                default:
                    break;
            }
            BLONE_IOE("(e) drenagem: ObjAdd falhou para '%s': %s",
                      name.c_str(), motivo);
            setIoStatus("IMPORT_FAIL|" + item.sourcePath + "|" + motivo);
        }
    }
}

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

// ATOMIC selection snapshot for the UI (single source of truth): the Kotlin
// layer reads this instead of keeping a diverging copy of the selection.
// Layout: [editMode, selectionMode, multiSelect, hasActiveObject,
//          nVerts, nEdges, nFaces, hasValidSelection].
// Dead ids are NOT counted; hasValidSelection distinguishes an EMPTY
// selection (all zero) from an INVALID one (ids kept but dead).
std::vector<jint> selectionSnapshotLocked() {
    int counts[3] = {0, 0, 0};
    blone::sceneops::selectionCounts(gScene, counts);
    const blone::SceneObject* obj = gScene.activeObject();
    return {gScene.editMode ? 1 : 0,
            static_cast<jint>(gScene.selectionMode),
            gScene.multiSelect ? 1 : 0,
            obj ? 1 : 0,
            counts[0], counts[1], counts[2],
            blone::sceneops::hasValidSelection(gScene) ? 1 : 0};
}

jintArray toJIntArray(JNIEnv* env, const std::vector<jint>& v) {
    jintArray arr = env->NewIntArray(static_cast<jsize>(v.size()));
    if (!arr) return nullptr;
    env->SetIntArrayRegion(arr, 0, static_cast<jsize>(v.size()), v.data());
    return arr;
}

jfloatArray toJFloatArray(JNIEnv* env, const std::vector<jfloat>& v) {
    jfloatArray arr = env->NewFloatArray(static_cast<jsize>(v.size()));
    if (!arr) return nullptr;
    env->SetFloatArrayRegion(arr, 0, static_cast<jsize>(v.size()), v.data());
    return arr;
}

// Raw surface hit under the pixel (NO selection side effects) — the brush
// entry point used by Kotlin before starting a stroke. Uses the camera
// snapshot of the last rendered frame, exactly like pickAtPixel/gizmoHitAt
// (single projection source). Returns [hit(0/1), objIndex, wX, wY, wZ].
std::vector<jfloat> brushHitLocked(jfloat xPx, jfloat yPx) {
    blone::Renderer::SurfaceHit hit;
    const bool ok = gRenderer.surfaceHitAt(xPx, yPx, gScene, hit);
    if (!ok) return {0.0f, -1.0f, 0.0f, 0.0f, 0.0f};
    return {1.0f, static_cast<float>(hit.objectIndex), hit.world.x, hit.world.y,
            hit.world.z};
}

// Brush ring color policy: paint uses the brush albedo (immediate feedback
// of what will be painted); sculpt uses the amber of the other editing
// overlays. (Kept next to the stroke entry points for clarity.)

// Defensive hygiene after ANY scene mutation: selection ids that no longer
// exist in the mesh are removed everywhere (belt and braces — the scene
// layer already prunes its own paths). Keeps the renderer/UI and the
// selection single-source-of-truth coherent at every entry point.
void pruneSelectionsLocked() { blone::sceneops::pruneAllSelections(gScene); }

// Selection pivot in WORLD space — the SHARED rule (core/scene/GizmoPivot.h)
// used by the renderer too: drawing, hit-testing and drag mapping can never
// diverge.
blone::Vec3 gizmoPivotForJni(const blone::Scene& scene, int& objIndexOut) {
    blone::Vec3 pivot{};
    if (!blone::gizmopivot::pivotFor(scene, scene.editMode, pivot, objIndexOut)) {
        objIndexOut = -1;
        return blone::Vec3{};
    }
    return pivot;
}

// Pixel-space mapping of a world axis at the pivot for gizmo drags: screen
// direction (y down), apparent pixels-per-world-unit and the ring sign that
// makes the surface point under the finger follow the finger.
//
// The projection uses the SAME usable viewport rect the renderer drew with
// (surfaceChanged + setViewportInsets), so the mapping can never diverge
// from what is on screen. The Kotlin-provided viewportW/H are only a
// fallback while the renderer has not been sized yet.
blone::sceneops::GizmoMapping gizmoMapping(const blone::OrbitCamera& cam,
                                           int axis, float viewportW,
                                           float viewportH) {
    blone::sceneops::GizmoMapping map;
    const blone::Vec3 axes[3] = {blone::Vec3{1, 0, 0}, blone::Vec3{0, 1, 0},
                                 blone::Vec3{0, 0, 1}};
    const blone::Vec3 axisDir = (axis >= 0 && axis <= 2) ? axes[axis] : axes[0];

    const float surfaceW = static_cast<float>(gRenderer.viewportSurfaceWidth());
    const float usableTop = static_cast<float>(gRenderer.viewportUsableTop());
    const float usableLeft = static_cast<float>(gRenderer.viewportUsableLeft());
    const float usableH = static_cast<float>(
            gRenderer.viewportUsableHeight() > 0 ? gRenderer.viewportUsableHeight()
                                                 : gRenderer.viewportSurfaceHeight());
    const float usableW = static_cast<float>(
            gRenderer.viewportUsableWidth() > 0 ? gRenderer.viewportUsableWidth()
                                                : gRenderer.viewportSurfaceWidth());
    const float vpW = surfaceW > 0.0f ? usableW : viewportW;
    const float vpH = usableH > 0.0f ? usableH : viewportH;
    const float vpTop = surfaceW > 0.0f ? usableTop : 0.0f;
    const float vpLeft = surfaceW > 0.0f ? usableLeft : 0.0f;

    // Selection pivot in world space (same rule as the renderer's gizmo).
    int objIndex = -1;
    blone::Vec3 pivot = gizmoPivotForJni(gScene, objIndex);
    const float vpAxisLen = 1.0f; // project a 1-unit step of the axis

    const blone::Mat4 proj = blone::Mat4::perspective(
            cam.fovYDeg,
            vpH > 0.0f ? vpW / vpH : 1.0f, 0.1f, 200.0f);
    const blone::Mat4 view = cam.viewMatrix();
    const blone::Mat4 vp = proj * view;

    auto projPx = [&](const blone::Vec3& w, float& sx, float& sy) -> bool {
        const float cx = vp.m[0] * w.x + vp.m[4] * w.y + vp.m[8] * w.z + vp.m[12];
        const float cy = vp.m[1] * w.x + vp.m[5] * w.y + vp.m[9] * w.z + vp.m[13];
        const float cw = vp.m[3] * w.x + vp.m[7] * w.y + vp.m[11] * w.z + vp.m[15];
        if (cw <= 1e-6f) return false;
        sx = vpLeft + ((cx / cw) * 0.5f + 0.5f) * vpW;
        sy = vpTop + (1.0f - ((cy / cw) * 0.5f + 0.5f)) * vpH;
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

// Reserves the top/bottom pixel bands for the Compose chrome so the model is
// projected into the really visible area. MUST be called on the GL thread
// (the Kotlin side dispatches it through queueEvent).
extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_setViewportInsets(JNIEnv* /*env*/, jobject /*thiz*/,
                                                  jint topPx, jint bottomPx) {
    std::lock_guard<std::mutex> lock(gMutex);
    gRenderer.setViewportInsets(topPx, bottomPx);
}

// Same, with left/right bands (side toolbar / property panel). MUST be called
// on the GL thread (queueEvent) — the usable rect must match the drawn frame.
extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_setViewportInsetsRect(JNIEnv* /*env*/, jobject /*thiz*/,
                                                      jint topPx, jint bottomPx,
                                                      jint leftPx, jint rightPx) {
    std::lock_guard<std::mutex> lock(gMutex);
    gRenderer.setViewportInsetsRect(topPx, bottomPx, leftPx, rightPx);
}

// Density scale for the gizmo touch targets (44 dp rule). Not GL state —
// safe from any thread (mutex-guarded).
extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_setGizmoTouchScale(JNIEnv* /*env*/, jobject /*thiz*/,
                                                   jfloat scale) {
    std::lock_guard<std::mutex> lock(gMutex);
    gRenderer.setGizmoTouchScale(scale);
}

// ATOMIC selection snapshot for the UI (single source of truth):
// [editMode, selectionMode, multiSelect, hasActive, nV, nE, nF, hasValid].
extern "C" JNIEXPORT jintArray JNICALL
Java_com_blone_app_gl_NativeLib_selectionSnapshot(JNIEnv* env, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return toJIntArray(env, selectionSnapshotLocked());
}

extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_drawFrame(JNIEnv* /*env*/, jobject /*thiz*/,
                                          jfloat azimuthDeg, jfloat elevationDeg,
                                          jfloat distance, jfloat tx, jfloat ty,
                                          jfloat tz, jfloat pointRadiusPx) {
    std::lock_guard<std::mutex> lock(gMutex);
    // Fase 4: drain finished imports BEFORE rendering — the new object is
    // visible the same frame (SceneOps facade bumps the scene version).
    drainPendingImportsLocked();
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

// Gizmo tool: 0 none ("Seleção"), 1 move, 2 rotate, 3 scale.
extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_setGizmoTool(JNIEnv* /*env*/, jobject /*thiz*/, jint tool) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (tool >= 0 && tool <= 3) {
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

// ======================== Fase 3: Esculpir / Pintar ========================
//
// Threading: same contract as the gizmo drag — UI thread, CPU-only, mutex
// guarded. The mesh NEVER crosses to Kotlin: the surface hit, the affected
// vertices, the deformation and the history entry all live in C++; the
// renderer resyncs from Scene::version on the next frame.

// Surface hit under the pixel: [hit(0/1), objIndex, wX, wY, wZ].
extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_blone_app_gl_NativeLib_brushHit(JNIEnv* env, jobject /*thiz*/,
                                         jfloat xPx, jfloat yPx) {
    std::lock_guard<std::mutex> lock(gMutex);
    return toJFloatArray(env, brushHitLocked(xPx, yPx));
}

// Starts a stroke and applies the first dab. tool: 0 sculpt, 1 paint.
// mode: 0 raise, 1 dig (sculpt only). r/g/b in [0,1] (paint only).
// Returns 0 (OK) or a negative status code.
extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_beginBrushStroke(JNIEnv* /*env*/, jobject /*thiz*/,
                                                 jint tool, jfloat xPx, jfloat yPx,
                                                 jfloat radius, jfloat strength,
                                                 jint mode, jfloat r, jfloat g,
                                                 jfloat b, jfloat opacity) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (tool != 0 && tool != 1) {
        return static_cast<jint>(blone::ops::Status::ErrInvalidArg);
    }
    blone::Renderer::SurfaceHit hit;
    if (!gRenderer.surfaceHitAt(xPx, yPx, gScene, hit)) {
        return static_cast<jint>(blone::ops::Status::ErrInvalidArg);
    }
    const blone::Vec3 hitNormal =
            gScene.objects[static_cast<size_t>(hit.objectIndex)].mesh.faceNormal(
                    hit.faceId);
    blone::brush::BrushSettings s;
    s.radius = radius;
    s.strength = strength;
    s.opacity = opacity;
    s.mode = (mode == 1) ? blone::brush::SculptMode::Dig
                         : blone::brush::SculptMode::Raise;
    s.color = blone::Vec3{r, g, b};
    const blone::sceneops::BrushTool bt =
            (tool == 0) ? blone::sceneops::BrushTool::Sculpt
                        : blone::sceneops::BrushTool::Paint;
    const jint rc = static_cast<jint>(blone::sceneops::beginBrushStroke(
            gScene, gHistory, gBrush, bt, hit.objectIndex, hit.local, hitNormal,
            s));
    if (rc == 0) {
        // Ring at the actual first application point; paint shows the brush
        // albedo, sculpt the amber of the other editing overlays.
        const bool paint = (bt == blone::sceneops::BrushTool::Paint);
        gRenderer.setBrushOverlay(true, hit.world, gBrush.settings.radius,
                                  paint ? gBrush.settings.color.x : 1.0f,
                                  paint ? gBrush.settings.color.y : 0.72f,
                                  paint ? gBrush.settings.color.z : 0.20f,
                                  0.95f);
    }
    return rc;
}

// Continues the stroke at the current finger position. A miss (finger slid
// off the mesh) keeps the session alive and only hides the ring.
extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_moveBrushStroke(JNIEnv* /*env*/, jobject /*thiz*/,
                                                jfloat xPx, jfloat yPx,
                                                jfloat radius, jfloat strength) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gBrush.active) {
        return static_cast<jint>(blone::ops::Status::ErrInvalidArg);
    }
    blone::Renderer::SurfaceHit hit;
    if (!gRenderer.surfaceHitAt(xPx, yPx, gScene, hit)) {
        gRenderer.setBrushOverlay(false, blone::Vec3{}, 0.0f, 0, 0, 0, 0);
        return 0; // off-surface: valid no-op, stroke continues
    }
    const blone::Vec3 hitNormal =
            gScene.objects[static_cast<size_t>(hit.objectIndex)].mesh.faceNormal(
                    hit.faceId);
    const jint rc = static_cast<jint>(blone::sceneops::moveBrushStroke(
            gScene, gBrush, hit.local, hitNormal, radius, strength));
    if (rc == 0) {
        const bool paint =
                (gBrush.tool == blone::sceneops::BrushTool::Paint);
        gRenderer.setBrushOverlay(true, hit.world, gBrush.settings.radius,
                                  paint ? gBrush.settings.color.x : 1.0f,
                                  paint ? gBrush.settings.color.y : 0.72f,
                                  paint ? gBrush.settings.color.z : 0.20f,
                                  0.95f);
    }
    return rc;
}

// Ends the stroke: commits exactly ONE history entry when something changed.
extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_endBrushStroke(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    const jint rc = static_cast<jint>(
            blone::sceneops::endBrushStroke(gScene, gHistory, gBrush));
    gRenderer.setBrushOverlay(false, blone::Vec3{}, 0.0f, 0, 0, 0, 0);
    return rc;
}

// Cancels the stroke: restores the captured start state (mesh and colors).
extern "C" JNIEXPORT void JNICALL
Java_com_blone_app_gl_NativeLib_cancelBrushStroke(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    blone::sceneops::cancelBrushStroke(gScene, gHistory, gBrush);
    gRenderer.setBrushOverlay(false, blone::Vec3{}, 0.0f, 0, 0, 0, 0);
}

// ======================== Fase 4: I/O OBJ/glTF =============================
//
// Kotlin resolves SAF to real file paths BEFORE calling these. Both entry
// points only VALIDATE arguments and enqueue a worker — they never parse,
// build or write inline (no disk I/O on the GL/UI threads, Regra 3).

// Sanitizes an import display name: printable ASCII only, no separators,
// non-empty (JNI strings are modified-UTF-8 — do not gamble on exotic input).
static std::string sanitizeImportName(const char* raw) {
    std::string in = raw ? raw : "";
    std::string out;
    for (const char c : in) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u <= 0x7E && c != '/' && c != '\\') out.push_back(c);
    }
    if (out.empty()) out = "Importado";
    return out;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_blone_app_gl_NativeLib_importMesh(JNIEnv* env, jobject /*thiz*/,
                                           jstring path, jboolean isGltf,
                                           jstring name) {
    const char* pathChars = env->GetStringUTFChars(path, nullptr);
    if (!pathChars) return JNI_FALSE;
    const std::string filePath(pathChars);
    env->ReleaseStringUTFChars(path, pathChars);
    const char* nameChars = env->GetStringUTFChars(name, nullptr);
    if (!nameChars) return JNI_FALSE;
    const std::string displayName = sanitizeImportName(nameChars);
    env->ReleaseStringUTFChars(name, nameChars);

    if (filePath.empty()) return JNI_FALSE;

    pruneFinishedFutures();
    std::lock_guard<std::mutex> lock(gIoMutex);
    gIoFutures.push_back(std::async(std::launch::async,
                                    [filePath, isGltf, displayName]() {
                                        // Worker: parse + build OFF the GL/UI
                                        // threads; the scene is NEVER touched
                                        // here — only the queue and the
                                        // status slot are.
                                        // Stage (b): the worker opens the
                                        // SAME path Kotlin copied — the size
                                        // in logcat exposes a truncated/empty
                                        // SAF copy at a glance.
                                        std::ifstream probe(filePath,
                                                            std::ios::binary |
                                                            std::ios::ate);
                                        if (!probe) {
                                            BLONE_IOE("import '%s': (b) worker "
                                                      "não abriu o path",
                                                      filePath.c_str());
                                            setIoStatus("IMPORT_FAIL|" +
                                                        filePath +
                                                        "|ficheiro não encontrado "
                                                        "no armazenamento");
                                            return;
                                        }
                                        const long long bytes =
                                                static_cast<long long>(
                                                        probe.tellg());
                                        probe.close();
                                        (void)bytes; // diagnostic only (logcat)
                                        BLONE_IOI("import '%s': (b) worker abriu "
                                                  "o ficheiro (%lld bytes, %s)",
                                                  filePath.c_str(), bytes,
                                                  isGltf ? "glTF/GLB"
                                                         : "OBJ");

                                        // Stage (c): parser with the PT error
                                        // code (reason flows to the toast).
                                        std::string err;
                                        std::optional<blone::io::RawMeshData>
                                                raw = isGltf == JNI_TRUE
                                                          ? blone::io::GltfLoader::load(
                                                                  filePath, &err)
                                                          : blone::io::ObjLoader::load(
                                                                  filePath, &err);
                                        if (!raw) {
                                            if (err.empty()) {
                                                err = "formato não reconhecido";
                                            }
                                            BLONE_IOE("import '%s': (c) parser "
                                                      "falhou: %s",
                                                      filePath.c_str(),
                                                      err.c_str());
                                            setIoStatus("IMPORT_FAIL|" +
                                                        filePath + "|" + err);
                                            return;
                                        }
                                        BLONE_IOI("import '%s': (c) parser OK "
                                                  "(V=%zu faces=%zu)",
                                                  filePath.c_str(),
                                                  raw->positions.size(),
                                                  raw->faceIndices.size());

                                        // Stage (d): MeshBuilder (all-or-nothing).
                                        std::optional<blone::HalfEdgeMesh>
                                                mesh = blone::io::MeshBuilder::build(
                                                        *raw, &err);
                                        if (!mesh) {
                                            if (err.empty()) {
                                                err = "geometria inválida";
                                            } else {
                                                err = "geometria inválida: " + err;
                                            }
                                            BLONE_IOE("import '%s': (d) "
                                                      "MeshBuilder falhou: %s",
                                                      filePath.c_str(),
                                                      err.c_str());
                                            setIoStatus("IMPORT_FAIL|" +
                                                        filePath + "|" + err);
                                            return;
                                        }
                                        BLONE_IOI("import '%s': (d) malha "
                                                  "construída e validada — "
                                                  "em fila para a drenagem",
                                                  filePath.c_str());
                                        {
                                            std::lock_guard<std::mutex> q(
                                                    gIoMutex);
                                            gPendingImports.push_back(
                                                    PendingImport{displayName,
                                                                  filePath,
                                                                  std::move(
                                                                          *mesh)});
                                        }
                                        // NOTE: no IMPORT_OK here — the
                                        // drain reports the final state once
                                        // the object REALLY entered the scene
                                        // (kMaxObjects/budget can still fail
                                        // at drain time).
                                    }));
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_blone_app_gl_NativeLib_exportObj(JNIEnv* env, jobject /*thiz*/,
                                          jstring path) {
    const char* pathChars = env->GetStringUTFChars(path, nullptr);
    if (!pathChars) return JNI_FALSE;
    const std::string filePath(pathChars);
    env->ReleaseStringUTFChars(path, pathChars);
    if (filePath.empty()) return JNI_FALSE;

    pruneFinishedFutures();
    std::lock_guard<std::mutex> lock(gIoMutex);
    gIoFutures.push_back(std::async(std::launch::async, [filePath]() {
        // Deep snapshot under gMutex (captureRaw/restoreRaw per object —
        // Raw carries vertex colors since Fase 3). The worker writes the
        // file ONLY over the snapshot; the live scene is untouched.
        blone::Scene snapshot;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            snapshot.objects.resize(gScene.objects.size());
            for (size_t i = 0; i < gScene.objects.size(); ++i) {
                snapshot.objects[i].name = gScene.objects[i].name;
                snapshot.objects[i].position = gScene.objects[i].position;
                snapshot.objects[i].mesh.restoreRaw(
                        gScene.objects[i].mesh.captureRaw());
            }
        }
        const bool ok = blone::io::ObjExporter::exportScene(snapshot, filePath);
        if (ok) {
            BLONE_IOI("export: OBJ escrito em %s", filePath.c_str());
        } else {
            BLONE_IOE("export: falha ao escrever %s", filePath.c_str());
        }
        setIoStatus(std::string(ok ? "EXPORT_OK|" : "EXPORT_FAIL|") + filePath +
                    (ok ? "" : "|não foi possível escrever o ficheiro OBJ"));
    }));
    return JNI_TRUE;
}

// Atomic I/O status for Kotlin: peek (ioStatus) and read-and-clear
// (ioStatusConsume). Both are plain JNI reads — safe from any thread.
extern "C" JNIEXPORT jstring JNICALL
Java_com_blone_app_gl_NativeLib_ioStatus(JNIEnv* env, jobject /*thiz*/) {
    const std::string s = peekIoStatus();
    return env->NewStringUTF(s.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_blone_app_gl_NativeLib_ioStatusConsume(JNIEnv* env, jobject /*thiz*/) {
    const std::string s = takeIoStatus();
    return env->NewStringUTF(s.c_str());
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
    pruneSelectionsLocked();
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

// World-space centroid of the object (same whole-object rule as the gizmo
// pivot) for the properties panel readout; null when the index is invalid.
extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_blone_app_gl_NativeLib_objectCenter(JNIEnv* env, jobject /*thiz*/, jint index) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (index < 0 || index >= static_cast<jint>(gScene.objects.size())) return nullptr;
    const blone::SceneObject& obj = gScene.objects[static_cast<size_t>(index)];
    if (obj.mesh.empty()) return nullptr;
    blone::Vec3 acc{};
    size_t count = 0;
    for (const auto& v : obj.mesh.vertices()) {
        if (!v.alive) continue;
        acc = acc + v.pos;
        ++count;
    }
    if (count == 0) return nullptr;
    const blone::Vec3 center = acc * (1.0f / static_cast<float>(count)) + obj.position;
    const float c[3] = {center.x, center.y, center.z};
    jfloatArray arr = env->NewFloatArray(3);
    if (!arr) return nullptr;
    env->SetFloatArrayRegion(arr, 0, 3, c);
    return arr;
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
    const jint rc = static_cast<jint>(blone::sceneops::extrude(gScene, gHistory));
    pruneSelectionsLocked();
    return rc;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_subdivide(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    const jint rc = static_cast<jint>(blone::sceneops::subdivide(gScene, gHistory));
    pruneSelectionsLocked();
    return rc;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_deleteSelection(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    const jint rc = static_cast<jint>(blone::sceneops::deleteSelection(gScene, gHistory));
    pruneSelectionsLocked();
    return rc;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_deleteActiveObject(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    return static_cast<jint>(blone::sceneops::deleteActiveObject(gScene, gHistory));
}

// Duplicates the ACTIVE object; returns the new index or -status.
extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_duplicateActiveObject(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    const jint rc = static_cast<jint>(
            blone::sceneops::duplicateActiveObject(gScene, gHistory));
    pruneSelectionsLocked();
    return rc;
}

// Direct STEP transform for the properties panel: tool 1 move / 2 rotate /
// 3 scale; axis 0..2 (3 = uniform, scale only); amount in world units /
// degrees / factor. Returns 0 (OK) or a negative status code.
extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_transformDirect(JNIEnv* /*env*/, jobject /*thiz*/,
                                                jint tool, jint axis, jfloat amount) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (tool < 1 || tool > 3) return static_cast<jint>(blone::ops::Status::ErrInvalidArg);
    const jint rc = static_cast<jint>(blone::sceneops::transformDirect(
            gScene, gHistory, static_cast<blone::sceneops::Tool>(tool), axis, amount));
    pruneSelectionsLocked();
    return rc;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_loopCut(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    const jint rc = static_cast<jint>(blone::sceneops::loopCut(gScene, gHistory));
    pruneSelectionsLocked();
    return rc;
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
    const jint rc = static_cast<jint>(blone::sceneops::undo(gScene, gHistory));
    pruneSelectionsLocked();
    return rc;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_blone_app_gl_NativeLib_redo(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gMutex);
    const jint rc = static_cast<jint>(blone::sceneops::redo(gScene, gHistory));
    pruneSelectionsLocked();
    return rc;
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
