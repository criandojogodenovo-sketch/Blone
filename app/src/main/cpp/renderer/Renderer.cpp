#include "Renderer.h"

#include "../core/Log.h"
#include "../core/Scene.h"
#include "../core/render/RenderData.h"
#include "../core/scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace blone {

namespace {

const char* kFillVertexShader = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormal;
out vec3 vWorldPos;
void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(uModel) * aNormal;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

// Correção "faces invisíveis": além da luz principal existe um termo de
// preenchimento (fill) oposto — faces viradas para longe da luz mantêm
// leitura clara em vez de afundar no fundo (#0E1218). Sem desativar o
// culling: a correção é de iluminação, não de winding.
const char* kFillFragmentShader = R"(#version 300 es
precision mediump float;
in vec3 vNormal;
in vec3 vWorldPos;
uniform vec3 uLightDir;
uniform vec3 uEyePos;
uniform vec3 uTint;
uniform float uTintMix;
out vec4 fragColor;
void main() {
    vec3 base = vec3(0.58, 0.66, 0.78);
    vec3 n = normalize(vNormal);
    vec3 l = normalize(uLightDir);
    float diff = max(dot(n, l), 0.0);
    float fill = max(dot(n, -l), 0.0);       // luz de preenchimento oposta
    float ambient = 0.34;
    vec3 viewDir = normalize(uEyePos - vWorldPos);
    vec3 halfV = normalize(l + viewDir);
    float spec = pow(max(dot(n, halfV), 0.0), 32.0) * 0.30;
    vec3 color = base * (ambient + diff * 0.70 + fill * 0.26) + vec3(spec);
    color = mix(color, uTint, uTintMix);
    fragColor = vec4(color, 1.0);
}
)";

const char* kLineVertexShader = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uMVP;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* kLineFragmentShader = R"(#version 300 es
precision mediump float;
in vec4 vColor;
uniform vec4 uOverride;
uniform float uUseOverride;
out vec4 fragColor;
void main() {
    fragColor = mix(vColor, uOverride, uUseOverride);
}
)";

const char* kPointVertexShader = R"(#version 300 es
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
uniform float uPointSize;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_PointSize = uPointSize;
}
)";

const char* kPointFragmentShader = R"(#version 300 es
precision mediump float;
uniform vec4 uColor;
out vec4 fragColor;
void main() {
    fragColor = uColor;
}
)";

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        BLONE_LOGE("Shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint linkProgram(const char* vsSource, const char* fsSource) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSource);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSource);
    if (vs == 0 || fs == 0) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        BLONE_LOGE("Program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

struct FillVertex {
    float px, py, pz, nx, ny, nz;
};
// LineVertex comes from core/Scene.h (pos + rgba).

// Cached object-mode outline in LOCAL space (built once per object; drawn
// with vp*model — no per-frame VAO/VBO allocation anymore).
std::vector<LineVertex> boxEdgesLines(const Aabb& b) {
    const float x0 = b.minX, y0 = b.minY, z0 = b.minZ;
    const float x1 = b.maxX, y1 = b.maxY, z1 = b.maxZ;
    const float c[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float p[8][3] = {
            {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1},
            {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1},
    };
    const int edges[12][2] = {
            {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
            {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    std::vector<LineVertex> out;
    out.reserve(24);
    for (const auto& e : edges) {
        out.push_back({p[e[0]][0], p[e[0]][1], p[e[0]][2], c[0], c[1], c[2], c[3]});
        out.push_back({p[e[1]][0], p[e[1]][1], p[e[1]][2], c[0], c[1], c[2], c[3]});
    }
    return out;
}

// Möller–Trumbore triangle intersection. Returns t or -1.
float rayTriangle(const Vec3& orig, const Vec3& dir, const Vec3& v0,
                  const Vec3& v1, const Vec3& v2) {
    const Vec3 e1 = v1 - v0;
    const Vec3 e2 = v2 - v0;
    const Vec3 p = cross(dir, e2);
    const float det = dot(e1, p);
    if (std::fabs(det) < 1e-9f) return -1.0f;
    const float inv = 1.0f / det;
    const Vec3 t = orig - v0;
    const float u = dot(t, p) * inv;
    if (u < -1e-6f || u > 1.0f + 1e-6f) return -1.0f;
    const Vec3 q = cross(t, e1);
    const float w = dot(dir, q) * inv;
    if (w < -1e-6f || u + w > 1.0f + 1e-6f) return -1.0f;
    const float tt = dot(e2, q) * inv;
    return (tt > 0.0f) ? tt : -1.0f;
}

constexpr float kMaxPickDepthSlack = 0.25f; // occlusion slack along the ray

// Gizmo palette (X red, Y green, Z blue).
constexpr float kAxisColor[3][3] = {
        {0.95f, 0.30f, 0.32f},
        {0.35f, 0.80f, 0.40f},
        {0.35f, 0.60f, 0.98f},
};
constexpr float kGizmoArrowPx = 78.0f;  // axis length on screen
constexpr float kGizmoRingPx = 54.0f;   // rotation ring radius on screen
constexpr float kGizmoTipRadiusPx = 26.0f;
constexpr float kGizmoRingRadiusPx = 16.0f;
constexpr float kGizmoCenterRadiusPx = 22.0f;

} // namespace

void Renderer::init() {
    if (fillProgram_ != 0) glDeleteProgram(fillProgram_);
    if (lineProgram_ != 0) glDeleteProgram(lineProgram_);
    if (pointProgram_ != 0) glDeleteProgram(pointProgram_);
    if (sceneVao_ != 0) glDeleteVertexArrays(1, &sceneVao_);
    if (sceneVbo_ != 0) glDeleteBuffers(1, &sceneVbo_);
    if (gizmoLineVao_ != 0) glDeleteVertexArrays(1, &gizmoLineVao_);
    if (gizmoLineVbo_ != 0) glDeleteBuffers(1, &gizmoLineVbo_);
    if (gizmoPtVao_ != 0) glDeleteVertexArrays(1, &gizmoPtVao_);
    if (gizmoPtVbo_ != 0) glDeleteBuffers(1, &gizmoPtVbo_);
    for (ObjectGpu& g : gpu_) destroyObjectGpu(g);
    gpu_.clear();
    sceneVersion_ = 0;

    fillProgram_ = linkProgram(kFillVertexShader, kFillFragmentShader);
    lineProgram_ = linkProgram(kLineVertexShader, kLineFragmentShader);
    pointProgram_ = linkProgram(kPointVertexShader, kPointFragmentShader);

    if (fillProgram_ != 0) {
        fillMvpLoc_ = glGetUniformLocation(fillProgram_, "uMVP");
        fillModelLoc_ = glGetUniformLocation(fillProgram_, "uModel");
        fillLightLoc_ = glGetUniformLocation(fillProgram_, "uLightDir");
        fillEyeLoc_ = glGetUniformLocation(fillProgram_, "uEyePos");
        fillTintLoc_ = glGetUniformLocation(fillProgram_, "uTint");
        fillTintMixLoc_ = glGetUniformLocation(fillProgram_, "uTintMix");
    }
    if (lineProgram_ != 0) {
        lineMvpLoc_ = glGetUniformLocation(lineProgram_, "uMVP");
        lineOverrideLoc_ = glGetUniformLocation(lineProgram_, "uOverride");
        lineUseOverrideLoc_ = glGetUniformLocation(lineProgram_, "uUseOverride");
    }
    if (pointProgram_ != 0) {
        pointMvpLoc_ = glGetUniformLocation(pointProgram_, "uMVP");
        pointColorLoc_ = glGetUniformLocation(pointProgram_, "uColor");
        pointSizeLoc_ = glGetUniformLocation(pointProgram_, "uPointSize");
    }

    buildSceneLines();

    // Gizmo dynamic buffers (small, orphaned per frame; scratch reused).
    glGenVertexArrays(1, &gizmoLineVao_);
    glGenBuffers(1, &gizmoLineVbo_);
    glBindVertexArray(gizmoLineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, gizmoLineVbo_);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    glGenVertexArrays(1, &gizmoPtVao_);
    glGenBuffers(1, &gizmoPtVbo_);
    glBindVertexArray(gizmoPtVao_);
    glBindBuffer(GL_ARRAY_BUFFER, gizmoPtVbo_);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.055f, 0.071f, 0.094f, 1.0f); // #0E1218
}

void Renderer::buildSceneLines() {
    SceneLines grid = buildGrid(0.0f, 15, 1);
    SceneLines axes = buildAxes(0.0f, 9.0f, 3.0f);
    std::vector<LineVertex> all;
    all.reserve(grid.vertices.size() + axes.vertices.size());
    all.insert(all.end(), grid.vertices.begin(), grid.vertices.end());
    all.insert(all.end(), axes.vertices.begin(), axes.vertices.end());
    sceneVertexCount_ = static_cast<GLsizei>(all.size());

    glGenVertexArrays(1, &sceneVao_);
    glGenBuffers(1, &sceneVbo_);
    glBindVertexArray(sceneVao_);
    glBindBuffer(GL_ARRAY_BUFFER, sceneVbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(all.size() * sizeof(LineVertex)),
                 all.data(), GL_STATIC_DRAW);
    const GLsizei stride = sizeof(LineVertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

void Renderer::destroyObjectGpu(ObjectGpu& g) {
    glDeleteVertexArrays(1, &g.vao);
    glDeleteBuffers(1, &g.vbo);
    glDeleteBuffers(1, &g.ebo);
    glDeleteVertexArrays(1, &g.wireVao);
    glDeleteBuffers(1, &g.wireVbo);
    glDeleteVertexArrays(1, &g.ptVao);
    glDeleteBuffers(1, &g.ptVbo);
    glDeleteVertexArrays(1, &g.boxVao);
    glDeleteBuffers(1, &g.boxVbo);
    glDeleteVertexArrays(1, &g.selFaceVao);
    glDeleteBuffers(1, &g.selFaceVbo);
    glDeleteBuffers(1, &g.selFaceEbo);
    glDeleteVertexArrays(1, &g.selEdgeVao);
    glDeleteBuffers(1, &g.selEdgeVbo);
    glDeleteVertexArrays(1, &g.selPtVao);
    glDeleteBuffers(1, &g.selPtVbo);
    g = ObjectGpu{};
}

void Renderer::buildObjectGeometry(ObjectGpu& g, const SceneObject& obj) {
    destroyObjectGpu(g);

    // Single geometric source (render == edit mesh == picking).
    renderdata::RenderData rd;
    renderdata::buildObject(obj, rd);
    g.bounds = rd.bounds;

    // Interleave pos+normal for the fill pass.
    std::vector<FillVertex> verts;
    verts.reserve(rd.fillVertexCount);
    for (size_t i = 0; i < rd.fillVertexCount; ++i) {
        verts.push_back({rd.fillPos[i * 3 + 0], rd.fillPos[i * 3 + 1],
                         rd.fillPos[i * 3 + 2], rd.fillNrm[i * 3 + 0],
                         rd.fillNrm[i * 3 + 1], rd.fillNrm[i * 3 + 2]});
    }

    // Fill buffers.
    glGenVertexArrays(1, &g.vao);
    glGenBuffers(1, &g.vbo);
    glGenBuffers(1, &g.ebo);
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(FillVertex)),
                 verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(rd.fillIdx.size() * sizeof(uint32_t)),
                 rd.fillIdx.data(), GL_STATIC_DRAW);
    const GLsizei stride = sizeof(FillVertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    g.indexCount = static_cast<GLsizei>(rd.fillIdx.size());

    // Wireframe.
    std::vector<LineVertex> wire;
    wire.reserve(rd.wirePos.size() / 3);
    for (size_t i = 0; i + 2 < rd.wirePos.size(); i += 3) {
        wire.push_back({rd.wirePos[i], rd.wirePos[i + 1], rd.wirePos[i + 2],
                        0.08f, 0.09f, 0.11f, 0.9f});
    }
    glGenVertexArrays(1, &g.wireVao);
    glGenBuffers(1, &g.wireVbo);
    glBindVertexArray(g.wireVao);
    glBindBuffer(GL_ARRAY_BUFFER, g.wireVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(wire.size() * sizeof(LineVertex)),
                 wire.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    g.wireCount = static_cast<GLsizei>(wire.size());

    // Vertex points (true editable vertices).
    glGenVertexArrays(1, &g.ptVao);
    glGenBuffers(1, &g.ptVbo);
    glBindVertexArray(g.ptVao);
    glBindBuffer(GL_ARRAY_BUFFER, g.ptVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(rd.pointPos.size() * sizeof(float)),
                 rd.pointPos.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    g.ptCount = static_cast<GLsizei>(rd.pointPos.size() / 3);

    // Object-mode outline, cached in local space.
    std::vector<LineVertex> box = boxEdgesLines(rd.bounds);
    glGenVertexArrays(1, &g.boxVao);
    glGenBuffers(1, &g.boxVbo);
    glBindVertexArray(g.boxVao);
    glBindBuffer(GL_ARRAY_BUFFER, g.boxVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(box.size() * sizeof(LineVertex)),
                 box.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    g.boxCount = static_cast<GLsizei>(box.size());
}

void Renderer::buildSelectionOverlays(ObjectGpu& g, const SceneObject& obj) {
    glDeleteVertexArrays(1, &g.selFaceVao);
    glDeleteBuffers(1, &g.selFaceVbo);
    glDeleteBuffers(1, &g.selFaceEbo);
    glDeleteVertexArrays(1, &g.selEdgeVao);
    glDeleteBuffers(1, &g.selEdgeVbo);
    glDeleteVertexArrays(1, &g.selPtVao);
    glDeleteBuffers(1, &g.selPtVbo);
    g.selFaceVao = g.selFaceVbo = g.selFaceEbo = 0;
    g.selEdgeVao = g.selEdgeVbo = 0;
    g.selPtVao = g.selPtVbo = 0;
    g.selFaceIndexCount = g.selEdgeCount = g.selPtCount = 0;

    renderdata::SelectionData sd;
    renderdata::buildSelection(obj, sd);

    if (!sd.faceIdx.empty()) {
        std::vector<FillVertex> fv;
        fv.reserve(sd.facePos.size() / 3);
        for (size_t i = 0; i + 2 < sd.facePos.size(); i += 3) {
            fv.push_back({sd.facePos[i], sd.facePos[i + 1], sd.facePos[i + 2],
                          sd.faceNrm[i], sd.faceNrm[i + 1], sd.faceNrm[i + 2]});
        }
        glGenVertexArrays(1, &g.selFaceVao);
        glGenBuffers(1, &g.selFaceVbo);
        glGenBuffers(1, &g.selFaceEbo);
        glBindVertexArray(g.selFaceVao);
        glBindBuffer(GL_ARRAY_BUFFER, g.selFaceVbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(fv.size() * sizeof(FillVertex)),
                     fv.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.selFaceEbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(sd.faceIdx.size() * sizeof(uint32_t)),
                     sd.faceIdx.data(), GL_STATIC_DRAW);
        const GLsizei stride = sizeof(FillVertex);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);
        g.selFaceIndexCount = static_cast<GLsizei>(sd.faceIdx.size());
    }

    if (!sd.edgePos.empty()) {
        std::vector<LineVertex> el;
        el.reserve(sd.edgePos.size() / 3);
        for (size_t i = 0; i + 2 < sd.edgePos.size(); i += 3) {
            el.push_back({sd.edgePos[i], sd.edgePos[i + 1], sd.edgePos[i + 2],
                          1.0f, 0.72f, 0.20f, 1.0f});
        }
        glGenVertexArrays(1, &g.selEdgeVao);
        glGenBuffers(1, &g.selEdgeVbo);
        glBindVertexArray(g.selEdgeVao);
        glBindBuffer(GL_ARRAY_BUFFER, g.selEdgeVbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(el.size() * sizeof(LineVertex)),
                     el.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                              reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                              reinterpret_cast<void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);
        g.selEdgeCount = static_cast<GLsizei>(el.size());
    }

    if (!sd.vertPos.empty()) {
        glGenVertexArrays(1, &g.selPtVao);
        glGenBuffers(1, &g.selPtVbo);
        glBindVertexArray(g.selPtVao);
        glBindBuffer(GL_ARRAY_BUFFER, g.selPtVbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(sd.vertPos.size() * sizeof(float)),
                     sd.vertPos.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                              reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
        g.selPtCount = static_cast<GLsizei>(sd.vertPos.size() / 3);
    }
}

void Renderer::syncObjects(const Scene& scene) {
    if (scene.version == sceneVersion_ && gpu_.size() == scene.objects.size()) {
        return;
    }
    gpu_.resize(scene.objects.size());
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        buildObjectGeometry(gpu_[i], scene.objects[i]);
        buildSelectionOverlays(gpu_[i], scene.objects[i]);
    }
    sceneVersion_ = scene.version;
}

void Renderer::resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    width_ = width;
    height_ = height;
    aspect_ = static_cast<float>(width) / static_cast<float>(height);
    glViewport(0, 0, width, height);
}

void Renderer::render(const OrbitCamera& camera, const Scene& scene,
                      float pointRadiusPx, GizmoTool gizmoTool) {
    if (fillProgram_ == 0 || lineProgram_ == 0 || pointProgram_ == 0) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        return;
    }
    proj_ = Mat4::perspective(camera.fovYDeg, aspect_, 0.1f, 200.0f);
    view_ = camera.viewMatrix();
    cam_ = camera;
    eye_ = camera.eye();

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    syncObjects(scene);

    const Mat4 vp = proj_ * view_;
    const Vec3 lightDir = normalize(Vec3{0.45f, 0.85f, 0.35f});

    // Grid + axes.
    glDepthMask(GL_FALSE);
    glUseProgram(lineProgram_);
    glUniformMatrix4fv(lineMvpLoc_, 1, GL_FALSE, vp.m);
    glUniform4f(lineOverrideLoc_, 1.0f, 1.0f, 1.0f, 1.0f);
    glUniform1f(lineUseOverrideLoc_, 0.0f);
    glBindVertexArray(sceneVao_);
    glDrawArrays(GL_LINES, 0, sceneVertexCount_);
    glDepthMask(GL_TRUE);

    // Objects.
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        const SceneObject& obj = scene.objects[i];
        if (obj.mesh.empty()) continue;
        const ObjectGpu& g = gpu_[i];
        const Mat4 model = Mat4::translate(obj.position.x, obj.position.y, obj.position.z);
        const Mat4 mvp = vp * model;

        glUseProgram(fillProgram_);
        glUniformMatrix4fv(fillMvpLoc_, 1, GL_FALSE, mvp.m);
        glUniformMatrix4fv(fillModelLoc_, 1, GL_FALSE, model.m);
        glUniform3f(fillLightLoc_, lightDir.x, lightDir.y, lightDir.z);
        glUniform3f(fillEyeLoc_, eye_.x, eye_.y, eye_.z);
        glUniform3f(fillTintLoc_, 0.0f, 0.0f, 0.0f);
        glUniform1f(fillTintMixLoc_, 0.0f);
        glBindVertexArray(g.vao);
        glDrawElements(GL_TRIANGLES, g.indexCount, GL_UNSIGNED_INT, nullptr);

        const bool active = (static_cast<int>(i) == scene.active);
        if (scene.editMode && active) {
            // Wireframe of the active object.
            glUseProgram(lineProgram_);
            glUniformMatrix4fv(lineMvpLoc_, 1, GL_FALSE, mvp.m);
            glUniform1f(lineUseOverrideLoc_, 0.0f);
            glBindVertexArray(g.wireVao);
            glDrawArrays(GL_LINES, 0, g.wireCount);

            // Selected faces (tinted, slightly pulled towards the camera).
            if (g.selFaceIndexCount > 0) {
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(-1.0f, -1.0f);
                glUseProgram(fillProgram_);
                glUniformMatrix4fv(fillMvpLoc_, 1, GL_FALSE, mvp.m);
                glUniform3f(fillTintLoc_, 1.0f, 0.72f, 0.20f);
                glUniform1f(fillTintMixLoc_, 0.55f);
                glBindVertexArray(g.selFaceVao);
                glDrawElements(GL_TRIANGLES, g.selFaceIndexCount, GL_UNSIGNED_INT,
                               nullptr);
                glDisable(GL_POLYGON_OFFSET_FILL);
            }
            // Selected edges + face-boundary highlight — drawn with depth
            // test OFF: selected elements always read on top (standard
            // modeling-app behaviour; kills z-fighting entirely).
            if (g.selEdgeCount > 0) {
                glDisable(GL_DEPTH_TEST);
                glUseProgram(lineProgram_);
                glUniformMatrix4fv(lineMvpLoc_, 1, GL_FALSE, mvp.m);
                glUniform1f(lineUseOverrideLoc_, 0.0f);
                glBindVertexArray(g.selEdgeVao);
                glDrawArrays(GL_LINES, 0, g.selEdgeCount);
                glEnable(GL_DEPTH_TEST);
            }
            // All vertices as small points; selected vertices bigger/amber.
            if (g.ptCount > 0) {
                glUseProgram(pointProgram_);
                glUniformMatrix4fv(pointMvpLoc_, 1, GL_FALSE, mvp.m);
                glUniform4f(pointColorLoc_, 0.55f, 0.62f, 0.72f, 1.0f);
                glUniform1f(pointSizeLoc_, pointRadiusPx * 0.45f);
                glBindVertexArray(g.ptVao);
                glDrawArrays(GL_POINTS, 0, g.ptCount);
            }
            if (g.selPtCount > 0) {
                glDisable(GL_DEPTH_TEST);
                glUseProgram(pointProgram_);
                glUniformMatrix4fv(pointMvpLoc_, 1, GL_FALSE, mvp.m);
                glUniform4f(pointColorLoc_, 1.0f, 0.72f, 0.20f, 1.0f);
                glUniform1f(pointSizeLoc_, pointRadiusPx);
                glBindVertexArray(g.selPtVao);
                glDrawArrays(GL_POINTS, 0, g.selPtCount);
                glEnable(GL_DEPTH_TEST);
            }
        } else if (active) {
            // Object mode: cached amber box outline (no per-frame allocation).
            glUseProgram(lineProgram_);
            glUniformMatrix4fv(lineMvpLoc_, 1, GL_FALSE, mvp.m);
            glUniform1f(lineUseOverrideLoc_, 0.0f);
            glBindVertexArray(g.boxVao);
            glDrawArrays(GL_LINES, 0, g.boxCount);
        }
    }
    glBindVertexArray(0);

    // Gizmo overlay (edit mode, active object) — always readable on top.
    if (scene.editMode) {
        drawGizmo(scene, vp, gizmoTool);
    }
}

// ------------------------------------------------------------------ gizmo --

bool Renderer::projectToScreenPx(const Vec3& world, float& sx, float& sy) const {
    const Mat4 vp = proj_ * view_;
    const float cx = vp.m[0] * world.x + vp.m[4] * world.y + vp.m[8] * world.z + vp.m[12];
    const float cy = vp.m[1] * world.x + vp.m[5] * world.y + vp.m[9] * world.z + vp.m[13];
    const float cw = vp.m[3] * world.x + vp.m[7] * world.y + vp.m[11] * world.z + vp.m[15];
    if (cw <= 1e-6f) return false; // behind the camera
    const float ndcX = cx / cw;
    const float ndcY = cy / cw;
    sx = (ndcX * 0.5f + 0.5f) * static_cast<float>(width_);
    sy = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(height_);
    return true;
}

namespace {

// Selection pivot in WORLD space: centroid of the selected elements, or of
// the whole object when the selection is empty (gizmo transforms the whole
// object then — the edit mode is never "locked").
Vec3 gizmoPivot(const Scene& scene, int& objIndexOut) {
    objIndexOut = scene.active;
    const SceneObject* obj = scene.activeObject();
    if (!obj || obj->mesh.empty()) return Vec3{};
    Vec3 acc{};
    size_t count = 0;
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
    if (count == 0) {
        for (const auto& v : obj->mesh.vertices()) {
            if (!v.alive) continue;
            acc = acc + v.pos;
            ++count;
        }
    }
    if (count == 0) return Vec3{};
    const Vec3 local = acc * (1.0f / static_cast<float>(count));
    return local + obj->position;
}

} // namespace

void Renderer::drawGizmo(const Scene& scene, const Mat4& vp, GizmoTool tool) {
    int objIndex = -1;
    const Vec3 pivot = gizmoPivot(scene, objIndex);
    if (objIndex < 0) return;

    const Vec3 toPivot = pivot - eye_;
    const float distPivot = std::max(length(toPivot), 0.5f);
    const float wpp = 2.0f * distPivot *
                      std::tan(cam_.fovYDeg * static_cast<float>(M_PI) / 180.0f * 0.5f) /
                      static_cast<float>(std::max(height_, 1));
    const float axisLen = wpp * kGizmoArrowPx;
    const float ringR = wpp * kGizmoRingPx;

    gizmoLineScratch_.clear();
    gizmoPtScratch_.clear();

    auto pushLine = [&](const Vec3& a, const Vec3& b, const float* c) {
        gizmoLineScratch_.push_back({a.x, a.y, a.z, c[0], c[1], c[2], 1.0f});
        gizmoLineScratch_.push_back({b.x, b.y, b.z, c[0], c[1], c[2], 1.0f});
    };

    const Vec3 axes[3] = {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
    Vec3 tips[3] = {pivot, pivot, pivot};

    if (tool == GizmoTool::Move) {
        for (int a = 0; a < 3; ++a) {
            tips[a] = pivot + axes[a] * axisLen;
            pushLine(pivot, tips[a], kAxisColor[a]);
        }
    } else if (tool == GizmoTool::Rotate) {
        for (int a = 0; a < 3; ++a) {
            const Vec3 helper = (std::fabs(axes[a].y) < 0.9f) ? Vec3{0, 1, 0}
                                                              : Vec3{1, 0, 0};
            const Vec3 u = normalize(cross(axes[a], helper));
            const Vec3 v = normalize(cross(axes[a], u));
            constexpr int kSegs = 48;
            Vec3 prev = pivot + (u * (ringR * std::cos(0.0f)) + v * (ringR * std::sin(0.0f)));
            for (int s = 1; s <= kSegs; ++s) {
                const float ang = static_cast<float>(M_PI) * 2.0f * static_cast<float>(s) /
                                  static_cast<float>(kSegs);
                const Vec3 cur = pivot + (u * (ringR * std::cos(ang)) + v * (ringR * std::sin(ang)));
                pushLine(prev, cur, kAxisColor[a]);
                prev = cur;
            }
        }
    } else { // Scale
        for (int a = 0; a < 3; ++a) {
            tips[a] = pivot + axes[a] * (axisLen * 0.85f);
            pushLine(pivot, tips[a], kAxisColor[a]);
        }
    }

    // Draw lines (depth test off — the gizmo always reads on top).
    if (!gizmoLineScratch_.empty()) {
        glDisable(GL_DEPTH_TEST);
        glBindVertexArray(gizmoLineVao_);
        glBindBuffer(GL_ARRAY_BUFFER, gizmoLineVbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(gizmoLineScratch_.size() * sizeof(LineVertex)),
                     gizmoLineScratch_.data(), GL_DYNAMIC_DRAW);
        glUseProgram(lineProgram_);
        glUniformMatrix4fv(lineMvpLoc_, 1, GL_FALSE, vp.m);
        glUniform1f(lineUseOverrideLoc_, 0.0f);
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(gizmoLineScratch_.size()));
        glBindVertexArray(0);
        glEnable(GL_DEPTH_TEST);
    }

    // Handle dots: one per axis tip (axis color) — readable touch targets.
    if (tool != GizmoTool::Rotate) {
        glDisable(GL_DEPTH_TEST);
        glBindVertexArray(gizmoPtVao_);
        glBindBuffer(GL_ARRAY_BUFFER, gizmoPtVbo_);
        glUseProgram(pointProgram_);
        glUniformMatrix4fv(pointMvpLoc_, 1, GL_FALSE, vp.m);
        glUniform1f(pointSizeLoc_, 12.0f);
        for (int a = 0; a < 3; ++a) {
            gizmoPtScratch_.clear();
            gizmoPtScratch_.push_back(tips[a].x);
            gizmoPtScratch_.push_back(tips[a].y);
            gizmoPtScratch_.push_back(tips[a].z);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(3 * sizeof(float)),
                         gizmoPtScratch_.data(), GL_DYNAMIC_DRAW);
            glUniform4f(pointColorLoc_, kAxisColor[a][0], kAxisColor[a][1],
                        kAxisColor[a][2], 1.0f);
            glDrawArrays(GL_POINTS, 0, 1);
        }
        if (tool == GizmoTool::Scale) {
            // Uniform-scale center handle (white dot).
            gizmoPtScratch_.clear();
            gizmoPtScratch_.push_back(pivot.x);
            gizmoPtScratch_.push_back(pivot.y);
            gizmoPtScratch_.push_back(pivot.z);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(3 * sizeof(float)),
                         gizmoPtScratch_.data(), GL_DYNAMIC_DRAW);
            glUniform4f(pointColorLoc_, 1.0f, 1.0f, 1.0f, 1.0f);
            glUniform1f(pointSizeLoc_, 10.0f);
            glDrawArrays(GL_POINTS, 0, 1);
        }
        glBindVertexArray(0);
        glEnable(GL_DEPTH_TEST);
    }
}

Renderer::GizmoHit Renderer::gizmoHitAt(float xPx, float yPx, const Scene& scene,
                                        GizmoTool tool) const {
    GizmoHit out;
    int objIndex = -1;
    const Vec3 pivot = gizmoPivot(scene, objIndex);
    if (objIndex < 0) return out;

    float psx = 0.0f, psy = 0.0f;
    if (!projectToScreenPx(pivot, psx, psy)) return out;

    const Vec3 toPivot = pivot - eye_;
    const float distPivot = std::max(length(toPivot), 0.5f);
    const float wpp = 2.0f * distPivot *
                      std::tan(cam_.fovYDeg * static_cast<float>(M_PI) / 180.0f * 0.5f) /
                      static_cast<float>(std::max(height_, 1));
    const float axisLen = wpp * kGizmoArrowPx;
    const float ringR = wpp * kGizmoRingPx;

    const Vec3 axes[3] = {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
    float best = std::numeric_limits<float>::max();

    auto consider = [&](float sx, float sy, int handle, int axis, int kind) {
        const float dx = sx - xPx;
        const float dy = sy - yPx;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d < best) {
            best = d;
            out.hit = true;
            out.handle = handle;
            out.axis = axis;
            out.kind = kind;
        }
    };

    if (tool == GizmoTool::Move || tool == GizmoTool::Scale) {
        for (int a = 0; a < 3; ++a) {
            const float scaleF = (tool == GizmoTool::Scale) ? 0.85f : 1.0f;
            const Vec3 tip = pivot + axes[a] * (axisLen * scaleF);
            float sx = 0.0f, sy = 0.0f;
            if (!projectToScreenPx(tip, sx, sy)) continue;
            const float dx = sx - xPx, dy = sy - yPx;
            if (std::sqrt(dx * dx + dy * dy) <= kGizmoTipRadiusPx) {
                consider(sx, sy, a + 1, a, 1);
            }
        }
        if (tool == GizmoTool::Scale) {
            const float dx = psx - xPx, dy = psy - yPx;
            if (std::sqrt(dx * dx + dy * dy) <= kGizmoCenterRadiusPx) {
                consider(psx, psy, 4, 3, 3);
            }
        }
    } else { // Rotate rings: sample each ring, min screen distance wins.
        for (int a = 0; a < 3; ++a) {
            const Vec3 helper = (std::fabs(axes[a].y) < 0.9f) ? Vec3{0, 1, 0}
                                                              : Vec3{1, 0, 0};
            const Vec3 u = normalize(cross(axes[a], helper));
            const Vec3 v = normalize(cross(axes[a], u));
            constexpr int kSamples = 64;
            for (int s = 0; s < kSamples; ++s) {
                const float ang = static_cast<float>(M_PI) * 2.0f * static_cast<float>(s) /
                                  static_cast<float>(kSamples);
                const Vec3 p = pivot + (u * (ringR * std::cos(ang)) + v * (ringR * std::sin(ang)));
                float sx = 0.0f, sy = 0.0f;
                if (!projectToScreenPx(p, sx, sy)) continue;
                const float dx = sx - xPx, dy = sy - yPx;
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d <= kGizmoRingRadiusPx && d < best) {
                    best = d;
                    out.hit = true;
                    out.handle = 11 + a;
                    out.axis = a;
                    out.kind = 2;
                }
            }
        }
    }
    return out;
}

// ----------------------------------------------------------------- picking --

Renderer::PickHit Renderer::pickAtPixel(float xPx, float yPx, const Scene& scene,
                                        float snapRadiusPx) const {
    PickHit out;
    if (width_ <= 0 || height_ <= 0) return out;

    const float ndcX = (2.0f * xPx / static_cast<float>(width_)) - 1.0f;
    const float ndcY = 1.0f - (2.0f * yPx / static_cast<float>(height_));
    const float tanHalf = std::tan(cam_.fovYDeg * static_cast<float>(M_PI) / 180.0f * 0.5f);
    const Vec3 dir = normalize(cam_.forward() +
                               cam_.right() * (ndcX * tanHalf * aspect_) +
                               cam_.up() * (ndcY * tanHalf));

    // 1) Nearest face hit across all objects — SAME triangulation the
    //    renderer draws (renderdata::faceTriangles), so what you tap is what
    //    you see (single geometric source).
    float bestT = std::numeric_limits<float>::max();
    int bestObj = -1;
    ElemId bestFace = kNull;
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        const SceneObject& obj = scene.objects[i];
        if (obj.mesh.empty()) continue;
        const Vec3 local = Vec3{eye_.x - obj.position.x, eye_.y - obj.position.y,
                                eye_.z - obj.position.z};
        for (const ElemId f : obj.mesh.aliveFaceIds()) {
            std::vector<ElemId> tris;
            renderdata::faceTriangles(obj.mesh, f, tris);
            for (size_t k = 0; k + 2 < tris.size(); k += 3) {
                const float t = rayTriangle(local, dir,
                                            obj.mesh.vrt(tris[k + 0]).pos,
                                            obj.mesh.vrt(tris[k + 1]).pos,
                                            obj.mesh.vrt(tris[k + 2]).pos);
                if (t > 0.0f && t < bestT) {
                    bestT = t;
                    bestObj = static_cast<int>(i);
                    bestFace = f;
                }
            }
        }
    }
    if (bestObj < 0) return out; // nothing visible under the finger

    const SceneObject& obj = scene.objects[static_cast<size_t>(bestObj)];

    // 2) Vertex/edge snapping across ALL alive elements of the object (fix:
    //    previously only the hit face's vertices/midpoints were considered),
    //    depth-checked against the visible surface (occlusion).
    if (scene.editMode) {
        auto depthAlong = [&](const Vec3& world) {
            return dot(world - eye_, dir);
        };

        // Vertices first (priority).
        float bestScreenV = snapRadiusPx;
        ElemId bestVert = kNull;
        for (const auto& v : obj.mesh.vertices()) {
            if (!v.alive) continue;
            const Vec3 w = v.pos + obj.position;
            float sx = 0.0f, sy = 0.0f;
            if (!projectToScreenPx(w, sx, sy)) continue;
            const float dx = sx - xPx, dy = sy - yPx;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d >= bestScreenV) continue;
            if (depthAlong(w) > bestT + kMaxPickDepthSlack) continue;
            bestScreenV = d;
            bestVert = v.id;
        }
        if (bestVert != kNull) {
            out.hit = true;
            out.kind = 1;
            out.objectIndex = bestObj;
            out.elemId = bestVert;
            return out;
        }

        // Edges: closest point ON THE SEGMENT in screen space (fix: the old
        // code only tested the midpoint, so tapping near an edge end missed).
        float bestScreenE = snapRadiusPx;
        ElemId bestEdge = kNull;
        for (const auto& e : obj.mesh.edges()) {
            if (!e.alive) continue;
            const ElemId h = e.he;
            const Vec3 a = obj.mesh.vrt(obj.mesh.heg(h).origin).pos + obj.position;
            const Vec3 b = obj.mesh.vrt(obj.mesh.heg(obj.mesh.heg(h).twin).origin).pos +
                           obj.position;
            float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
            if (!projectToScreenPx(a, ax, ay)) continue;
            if (!projectToScreenPx(b, bx, by)) continue;
            const float ex = bx - ax, ey = by - ay;
            const float len2 = ex * ex + ey * ey;
            float t = 0.0f;
            if (len2 > 1e-9f) {
                t = ((xPx - ax) * ex + (yPx - ay) * ey) / len2;
                t = std::clamp(t, 0.0f, 1.0f);
            }
            const float cxp = ax + ex * t;
            const float cyp = ay + ey * t;
            const float dx = cxp - xPx, dy = cyp - yPx;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d >= bestScreenE) continue;
            const Vec3 world = a + (b - a) * t;
            if (depthAlong(world) > bestT + kMaxPickDepthSlack) continue;
            bestScreenE = d;
            bestEdge = e.id;
        }
        if (bestEdge != kNull) {
            out.hit = true;
            out.kind = 2;
            out.objectIndex = bestObj;
            out.elemId = bestEdge;
            return out;
        }

        out.hit = true;
        out.kind = 3; // face
        out.objectIndex = bestObj;
        out.elemId = bestFace;
        return out;
    }

    out.hit = true;
    out.kind = 4; // object selection
    out.objectIndex = bestObj;
    out.elemId = bestFace;
    return out;
}

} // namespace blone
