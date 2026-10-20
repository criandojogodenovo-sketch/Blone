#pragma once
// OpenGL ES 3.0 renderer for the Blone viewport.
//
// Draws the static grid/axes plus one flat-shaded mesh per scene object
// (triangulation from the shared renderdata::faceTriangles source), with
// selection overlays for the active object in edit mode, and the EDIT-MODE
// GIZMO: X/Y/Z move arrows, rotation rings and scale handles drawn as a
// depth-independent overlay at the selection pivot, with a CPU hit-test so
// dragging a handle transforms the selection while dragging anywhere else
// orbits the camera.
//
// GPU buffers are rebuilt whenever Scene::version changes (documented
// trade-off: meshes are bounded by the Phase 2 geometry limits, so a full
// rebuild is cheap and always consistent).
//
// Also owns CPU picking: ray cast (Möller–Trumbore) against the SAME
// triangulation the renderer draws, then screen-space vertex/edge snapping
// across ALL alive elements of the hit object, depth-checked against the
// visible surface (fix for the displaced-marker report).

#include "../core/Camera.h"
#include "../core/Math3D.h"
#include "../core/Scene.h" // LineVertex (gizmo scratch buffers)
#include "../core/scene/Scene.h"

#include <GLES3/gl3.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace blone {

// Gizmo tool (mirrors the Kotlin Tool enum). None = no gizmo drawn and no
// gizmo hit (the "Seleção" tool: taps select, nothing transforms).
enum class GizmoTool : int { None = 0, Move = 1, Rotate = 2, Scale = 3 };

class Renderer {
public:
    void init(); // with a current GL context; safe to call again after loss
    void resize(int width, int height);
    // Reserves the top/bottom pixel bands for the UI chrome so the model is
    // projected and centered into the REALLY VISIBLE area (both
    // orientations). glViewport(0, bottom, width, height - top - bottom);
    // Must be called on the GL thread. Values are clamped; a degenerate
    // usable area falls back to the full viewport.
    void setViewportInsets(int topPx, int bottomPx);
    // Same, with LEFT/RIGHT bands too (side toolbars / property panel).
    // glViewport(left, bottom, width - left - right, height - top - bottom).
    void setViewportInsetsRect(int topPx, int bottomPx, int leftPx, int rightPx);
    // Density scale for the gizmo (Kotlin passes resources.displayMetrics
    // .density). Hit targets scale 1:1 with density (the 44 dp rule);
    // visual lengths scale moderately (capped) so the gizmo never explodes
    // on tablets. Clamped to [0.75, 3.0].
    void setGizmoTouchScale(float scale) {
        gizmoTouchScale_ = std::min(3.0f, std::max(0.75f,
                std::isfinite(scale) ? scale : 1.0f));
    }
    void render(const OrbitCamera& camera, const Scene& scene, float pointRadiusPx,
                GizmoTool gizmoTool);

    struct PickHit {
        bool hit = false;
        int kind = 0; // 0 none, 1 vertex, 2 edge, 3 face, 4 object
        int objectIndex = -1;
        ElemId elemId = kNull;
    };
    // Pure CPU picking using the camera snapshot of the last rendered frame.
    PickHit pickAtPixel(float xPx, float yPx, const Scene& scene,
                        float snapRadiusPx) const;

    // ---- brush (Fase 3) ---------------------------------------------------
    // Raw surface hit WITHOUT any selection side effect: nearest triangle
    // across all objects under the pixel (same ray + triangulation as
    // pickAtPixel). Returns the hit point in WORLD and LOCAL space.
    struct SurfaceHit {
        bool hit = false;
        int objectIndex = -1;
        ElemId faceId = kNull;
        Vec3 world{}; // world-space hit point
        Vec3 local{}; // world - object.position (mesh local space)
    };
    bool surfaceHitAt(float xPx, float yPx, const Scene& scene,
                      SurfaceHit& out) const;

    // Brush aiming ring: billboard circle at `world` with the REAL brush
    // radius (world units). Active only while a stroke is running (set/clear
    // by the JNI stroke functions). rgba = ring color (alpha blended).
    void setBrushOverlay(bool active, const Vec3& world, float radiusWorld,
                         float r, float g, float b, float a);

    // Draws the brush ring if active (called at the end of render()).
    void drawBrushRing();

    // ---- gizmo ------------------------------------------------------------
    struct GizmoHit {
        bool hit = false;
        int handle = 0; // 0 none; 1..3 axis tip; 4 center (uniform scale);
                        // 11..13 rotation ring around X/Y/Z
        int axis = -1;  // 0=X, 1=Y, 2=Z; 3 for uniform center
        int kind = 0;   // 0 none, 1 axis tip, 2 ring, 3 center
    };
    // Screen-space hit-test against the gizmo of the given tool. Uses the
    // camera snapshot of the last rendered frame. Pure CPU (no GL calls).
    GizmoHit gizmoHitAt(float xPx, float yPx, const Scene& scene,
                        GizmoTool tool) const;

    // Usable-viewport state for the JNI gizmo mapping (same projection as
    // the drawn frame — mapping and picking never diverge).
    int viewportSurfaceWidth() const { return width_; }
    int viewportSurfaceHeight() const { return height_; }
    int viewportUsableTop() const { return usableTop_; }
    int viewportUsableHeight() const { return usableHeight_; }
    int viewportUsableLeft() const { return usableLeft_; }
    int viewportUsableWidth() const { return usableWidth_; }

private:
    struct ObjectGpu {
        GLuint vao = 0, vbo = 0, ebo = 0;
        GLsizei indexCount = 0;
        GLuint wireVao = 0, wireVbo = 0;
        GLsizei wireCount = 0;
        GLuint ptVao = 0, ptVbo = 0;
        GLsizei ptCount = 0;
        // cached object-mode outline (local space; drawn with vp*model)
        GLuint boxVao = 0, boxVbo = 0;
        GLsizei boxCount = 0;
        // selection overlays (active object only)
        GLuint selFaceVao = 0, selFaceVbo = 0, selFaceEbo = 0;
        GLsizei selFaceIndexCount = 0;
        GLuint selEdgeVao = 0, selEdgeVbo = 0;
        GLsizei selEdgeCount = 0;
        GLuint selPtVao = 0, selPtVbo = 0;
        GLsizei selPtCount = 0;
        Aabb bounds{};
    };

    void buildPrograms();
    void buildSceneLines();
    void syncObjects(const Scene& scene);
    void destroyObjectGpu(ObjectGpu& g);
    void buildObjectGeometry(ObjectGpu& g, const SceneObject& obj);
    void buildSelectionOverlays(ObjectGpu& g, const SceneObject& obj);

    // Shared ray builder for picking AND the brush surface hit (single
    // projection source — they can never diverge).
    bool buildPickRay(float xPx, float yPx, Vec3& dirOut) const;

    // Gizmo drawing (depth-test off overlay) + shared scratch buffers.
    void drawGizmo(const Scene& scene, const Mat4& vp, GizmoTool tool);
    void drawLineList(const std::vector<LineVertex>& lines, const Mat4& mvp);
    void drawPointList(const std::vector<float>& pts, const Mat4& mvp,
                       float r, float g, float b, float sizePx);

    // Screen-space projection of a world point using the last frame's
    // matrices; returns false when the point is behind the camera.
    bool projectToScreenPx(const Vec3& world, float& sx, float& sy) const;

    GLuint fillProgram_ = 0, lineProgram_ = 0, pointProgram_ = 0;
    GLint fillMvpLoc_ = -1, fillModelLoc_ = -1, fillLightLoc_ = -1;
    GLint fillEyeLoc_ = -1, fillTintLoc_ = -1, fillTintMixLoc_ = -1;
    GLint fillUseVColLoc_ = -1;
    GLint lineMvpLoc_ = -1, lineOverrideLoc_ = -1, lineUseOverrideLoc_ = -1;
    GLint pointMvpLoc_ = -1, pointColorLoc_ = -1, pointSizeLoc_ = -1;

    GLuint sceneVao_ = 0, sceneVbo_ = 0;
    GLsizei sceneVertexCount_ = 0;

    std::vector<ObjectGpu> gpu_;
    uint32_t sceneVersion_ = 0;

    // Gizmo GPU resources + reused scratch (no per-frame allocation).
    GLuint gizmoLineVao_ = 0, gizmoLineVbo_ = 0;
    GLuint gizmoPtVao_ = 0, gizmoPtVbo_ = 0;
    std::vector<LineVertex> gizmoLineScratch_;
    std::vector<float> gizmoPtScratch_;

    Mat4 proj_{}, view_{};
    OrbitCamera cam_{}; // snapshot of the last rendered frame (picking/gizmo)
    Vec3 eye_{};
    int width_ = 0, height_ = 0;
    float aspect_ = 1.0f;

    // Brush overlay state (Fase 3) — plain members, mutex-guarded by the JNI
    // layer; drawn by drawBrushRing() at the end of render().
    bool brushOverlayActive_ = false;
    Vec3 brushOverlayWorld_{};
    float brushRadiusWorld_ = 0.0f;
    float brushRingRgba_[4] = {1.0f, 0.72f, 0.20f, 0.9f};
    // Usable (chrome-free) viewport rect inside the surface. The projection,
    // picking and the gizmo mapping all use THIS rect, so the model is
    // centered in the visible area and what you tap is what you see.
    int topInset_ = 0, bottomInset_ = 0;
    int leftInset_ = 0, rightInset_ = 0;
    int usableTop_ = 0, usableHeight_ = 0;
    int usableLeft_ = 0, usableWidth_ = 0;
    float gizmoTouchScale_ = 1.0f;
    void applyViewport();
};

} // namespace blone
