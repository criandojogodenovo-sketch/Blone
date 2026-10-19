#pragma once
// OpenGL ES 3.0 renderer for the Blone viewport (Phase 2).
//
// Draws the static grid/axes plus one flat-shaded, fan-triangulated mesh per
// scene object, with selection overlays (faces tinted, edges highlighted,
// vertex points) for the active object in edit mode. GPU buffers are rebuilt
// whenever Scene::version changes (documented trade-off: meshes are bounded
// by the Phase 2 geometry limits, so a full rebuild is cheap and always
// consistent).
//
// Also owns CPU picking: ray cast (Möller–Trumbore) against every object with
// occlusion, then screen-space vertex/edge selection around the hit point,
// depth-checked against the visible surface.

#include "../core/Camera.h"
#include "../core/Math3D.h"
#include "../core/scene/Scene.h"

#include <GLES3/gl3.h>
#include <vector>

namespace blone {

class Renderer {
public:
    void init(); // with a current GL context; safe to call again after loss
    void resize(int width, int height);
    void render(const OrbitCamera& camera, const Scene& scene, float pointRadiusPx);

    struct PickHit {
        bool hit = false;
        int kind = 0; // 0 none, 1 vertex, 2 edge, 3 face, 4 object
        int objectIndex = -1;
        ElemId elemId = kNull;
    };
    // Pure CPU picking using the camera snapshot of the last rendered frame.
    PickHit pickAtPixel(float xPx, float yPx, const Scene& scene,
                        float snapRadiusPx) const;

private:
    struct ObjectGpu {
        GLuint vao = 0, vbo = 0, ebo = 0;
        GLsizei indexCount = 0;
        GLuint wireVao = 0, wireVbo = 0;
        GLsizei wireCount = 0;
        GLuint ptVao = 0, ptVbo = 0;
        GLsizei ptCount = 0;
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

    // Screen-space projection of a world point using the last frame's
    // matrices; returns pixel coordinates (origin top-left).
    Vec3 projectToScreen(const Vec3& world) const;
    bool screenSpaceDistance(const Vec3& world, float xPx, float yPx,
                             float& distOut) const;

    GLuint fillProgram_ = 0, lineProgram_ = 0, pointProgram_ = 0;
    GLint fillMvpLoc_ = -1, fillModelLoc_ = -1, fillLightLoc_ = -1;
    GLint fillEyeLoc_ = -1, fillTintLoc_ = -1, fillTintMixLoc_ = -1;
    GLint lineMvpLoc_ = -1, lineOverrideLoc_ = -1, lineUseOverrideLoc_ = -1;
    GLint pointMvpLoc_ = -1, pointColorLoc_ = -1, pointSizeLoc_ = -1;

    GLuint sceneVao_ = 0, sceneVbo_ = 0;
    GLsizei sceneVertexCount_ = 0;

    std::vector<ObjectGpu> gpu_;
    uint32_t sceneVersion_ = 0;

    Mat4 proj_{}, view_{};
    OrbitCamera cam_{}; // snapshot of the last rendered frame (picking)
    Vec3 eye_{};
    int width_ = 0, height_ = 0;
    float aspect_ = 1.0f;
};

} // namespace blone
