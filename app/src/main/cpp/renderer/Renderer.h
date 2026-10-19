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
#include <vector>

namespace blone {

// Gizmo tool shown in edit mode (mirrors the Kotlin Tool enum).
enum class GizmoTool : int { Move = 1, Rotate = 2, Scale = 3 };

class Renderer {
public:
    void init(); // with a current GL context; safe to call again after loss
    void resize(int width, int height);
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
};

} // namespace blone
