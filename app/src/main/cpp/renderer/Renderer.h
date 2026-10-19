#pragma once
// OpenGL ES 3.0 renderer for the Blone viewport.
// Owns shader programs, static geometry buffers and per-frame drawing.

#include "core/Camera.h"
#include "core/Math3D.h"
#include "core/Mesh.h"
#include "core/Scene.h"

#include <GLES3/gl3.h>

namespace blone {

class Renderer {
public:
    // Compiles shaders and uploads static geometry. Must be called with a
    // current GL context; safe to call again after a context loss (it will
    // rebuild all GL objects).
    void init();

    void resize(int width, int height);

    // Draws one frame with the given camera pose.
    void render(const OrbitCamera& camera, bool selected);

    // CPU-side picking using the camera/viewport stored by the last render.
    bool pickAtPixel(float xPx, float yPx) const;

private:
    void buildPrograms();
    void buildCubeBuffers();
    void buildLineBuffers();
    void buildEdgeBuffer();

    // Cube program
    GLuint cubeProgram_ = 0;
    GLint cubeMvpLoc_ = -1;
    GLint cubeModelLoc_ = -1;
    GLint cubeLightDirLoc_ = -1;
    GLint cubeEyePosLoc_ = -1;
    GLint cubeSelectedLoc_ = -1;

    // Line program
    GLuint lineProgram_ = 0;
    GLint lineMvpLoc_ = -1;
    GLint lineOverrideLoc_ = -1;
    GLint lineUseOverrideLoc_ = -1;

    // Cube geometry
    GLuint cubeVao_ = 0;
    GLuint cubeVbo_ = 0;
    GLuint cubeEbo_ = 0;
    GLsizei cubeIndexCount_ = 0;

    // Scene lines (grid + axes)
    GLuint sceneVao_ = 0;
    GLuint sceneVbo_ = 0;
    GLsizei sceneVertexCount_ = 0;

    // Selection edge highlight (cube box edges)
    GLuint edgeVao_ = 0;
    GLuint edgeVbo_ = 0;
    GLsizei edgeVertexCount_ = 0;

    CubeMesh cube_{};
    Aabb cubeBounds_{};

    // Last frame state for picking
    Vec3 eye_{};
    Vec3 forward_{};
    Vec3 right_{};
    Vec3 up_{};
    float fovYDeg_ = 45.0f;
    float aspect_ = 1.0f;
    int width_ = 0;
    int height_ = 0;
};

} // namespace blone
