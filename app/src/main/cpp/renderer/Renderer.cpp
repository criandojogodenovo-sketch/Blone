#include "Renderer.h"

#include "../core/Log.h"
#include "../core/Scene.h"
#include "../core/render/RenderData.h"
#include "../core/scene/GizmoPivot.h"
#include "../core/scene/Scene.h"
#include "../core/scene/SceneOps.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace blone {

namespace {

const char* kFillVertexShader = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormal;
out vec3 vWorldPos;
out vec3 vColor;
void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(uModel) * aNormal;
    vColor = aColor;
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
in vec3 vColor;
uniform vec3 uLightDir;
uniform vec3 uEyePos;
uniform vec3 uTint;
uniform float uTintMix;
// 1.0 = use the per-vertex albedo (mesh objects); 0.0 = historical base
// color (selection overlays, whose VAOs carry no color attribute).
uniform float uUseVertexColor;
// 1.0 = READ-MODE flat shading (missão F4-LIMITS, P3): the face normal is
// derived from screen-space derivatives of the world position — no normal
// attribute and no duplicated fill buffers are needed for view-only
// objects. dFdx/dFdy are core in ES 3.0.
uniform float uFlatDeriv;
out vec4 fragColor;
void main() {
    vec3 base = mix(vec3(0.58, 0.66, 0.78), clamp(vColor, 0.0, 1.0), uUseVertexColor);
    vec3 n = normalize(vNormal);
    if (uFlatDeriv > 0.5) {
        n = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));
    }
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
    float px, py, pz, nx, ny, nz, cr, cg, cb;
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
// VISUAL sizes (px at density 1; scaled moderately by gizmoTouchScale_,
// capped so the gizmo never explodes on tablets).
constexpr float kGizmoArrowPx = 78.0f;  // axis length on screen
constexpr float kGizmoRingPx = 54.0f;   // rotation ring radius on screen
// HIT targets (px at density 1; scaled 1:1 by gizmoTouchScale_): the tip
// radius gives a 44 px diameter at density 1 => the 44 dp touch-target rule
// (audit finding #3: the old fixed 26/16/22 px radii were far too small on
// high-density phones).
constexpr float kHitTipPx = 22.0f;      // axis tip => 44px diameter target
constexpr float kHitRingPx = 18.0f;     // rotation ring band
constexpr float kHitCenterPx = 20.0f;   // uniform-scale center => 40px
constexpr float kHitAxisSegmentPx = 10.0f; // anywhere along the axis body

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
        fillUseVColLoc_ = glGetUniformLocation(fillProgram_, "uUseVertexColor");
        fillFlatDerivLoc_ = glGetUniformLocation(fillProgram_, "uFlatDeriv");
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

    // Interleave pos+normal+color for the fill pass (Fase 3: vertex color).
    std::vector<FillVertex> verts;
    verts.reserve(rd.fillVertexCount);
    for (size_t i = 0; i < rd.fillVertexCount; ++i) {
        verts.push_back({rd.fillPos[i * 3 + 0], rd.fillPos[i * 3 + 1],
                         rd.fillPos[i * 3 + 2], rd.fillNrm[i * 3 + 0],
                         rd.fillNrm[i * 3 + 1], rd.fillNrm[i * 3 + 2],
                         rd.fillCol[i * 3 + 0], rd.fillCol[i * 3 + 1],
                         rd.fillCol[i * 3 + 2]});
    }

    // Fill buffers. Index type per object (P2): 16-bit IBO when the
    // duplicated fill vertex count allows it, uint32 otherwise (verified —
    // a mismatched downcast falls back to uint32, never a truncated IBO).
    glGenVertexArrays(1, &g.vao);
    glGenBuffers(1, &g.vbo);
    glGenBuffers(1, &g.ebo);
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(FillVertex)),
                 verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
    if (renderdata::fitsUint16Index(rd.fillVertexCount)) {
        std::vector<uint16_t> idx16;
        renderdata::downcastIndices16(rd.fillIdx, idx16);
        if (idx16.size() == rd.fillIdx.size()) {
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(idx16.size() * sizeof(uint16_t)),
                         idx16.data(), GL_STATIC_DRAW);
            g.indexType = GL_UNSIGNED_SHORT;
        } else {
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(rd.fillIdx.size() * sizeof(uint32_t)),
                         rd.fillIdx.data(), GL_STATIC_DRAW);
            g.indexType = GL_UNSIGNED_INT;
        }
    } else {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(rd.fillIdx.size() * sizeof(uint32_t)),
                     rd.fillIdx.data(), GL_STATIC_DRAW);
        g.indexType = GL_UNSIGNED_INT;
    }
    const GLsizei stride = sizeof(FillVertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
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
                          sd.faceNrm[i], sd.faceNrm[i + 1], sd.faceNrm[i + 2],
                          0.0f, 0.0f, 0.0f}); // color unused (uUseVertexColor=0)
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

void Renderer::buildReadGeometry(ObjectGpu& g, const SceneObject& obj) {
    const auto* rm = obj.readMesh.get();
    if (!rm || rm->empty()) return;
    // Re-upload skip: the RenderMesh is immutable and shared — the buffers
    // only change when the object's geometry identity changes (import,
    // promotion, undo/redo swap).
    if (g.readUploaded == static_cast<const void*>(rm) && g.indexCount > 0) {
        return;
    }
    destroyObjectGpu(g);

    // Same FillVertex layout as editable objects (shared upload path, P7
    // pool compatible); the normal attribute is unused in read mode (the
    // fragment shader derives the face normal — uFlatDeriv=1).
    const size_t n = rm->positions.size();
    const bool hasColors = rm->colors.size() == n;
    std::vector<FillVertex> verts;
    verts.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const Vec3& p = rm->positions[i];
        const Vec3 c = hasColors ? rm->colors[i] : Vec3{1.0f, 1.0f, 1.0f};
        verts.push_back({p.x, p.y, p.z, 0.0f, 0.0f, 0.0f, c.x, c.y, c.z});
    }

    glGenVertexArrays(1, &g.vao);
    glGenBuffers(1, &g.vbo);
    glGenBuffers(1, &g.ebo);
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(FillVertex)),
                 verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
    // Same per-object index rule as P2 (uint16 up to 65535, uint32 above —
    // a 1M-triangle view mesh has millions of indices).
    if (renderdata::fitsUint16Index(n)) {
        std::vector<uint16_t> idx16;
        renderdata::downcastIndices16(rm->indices, idx16);
        if (idx16.size() == rm->indices.size()) {
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(idx16.size() * sizeof(uint16_t)),
                         idx16.data(), GL_STATIC_DRAW);
            g.indexType = GL_UNSIGNED_SHORT;
        }
    }
    if (g.indexType != GL_UNSIGNED_SHORT) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(rm->indices.size() * sizeof(uint32_t)),
                     rm->indices.data(), GL_STATIC_DRAW);
        g.indexType = GL_UNSIGNED_INT;
    }
    const GLsizei stride = sizeof(FillVertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    g.indexCount = static_cast<GLsizei>(rm->indices.size());
    g.bounds = rm->bounds;
    g.readUploaded = static_cast<const void*>(rm);

    // Object-mode outline from the read bounds (same cached-local-space
    // scheme as editable objects).
    std::vector<LineVertex> box = boxEdgesLines(rm->bounds);
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

void Renderer::syncObjects(const Scene& scene) {
    if (scene.version == sceneVersion_ && gpu_.size() == scene.objects.size()) {
        return;
    }
    gpu_.resize(scene.objects.size());
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        const SceneObject& obj = scene.objects[i];
        if (!obj.editable) {
            buildReadGeometry(gpu_[i], obj); // re-upload skip inside
        } else {
            buildObjectGeometry(gpu_[i], obj);
            buildSelectionOverlays(gpu_[i], obj);
        }
    }
    sceneVersion_ = scene.version;
}

void Renderer::applyViewport() {
    if (width_ <= 0 || height_ <= 0) return;
    int top = std::clamp(topInset_, 0, height_ - 1);
    int bottom = std::clamp(bottomInset_, 0, height_ - 1 - top);
    int left = std::clamp(leftInset_, 0, width_ - 1);
    int right = std::clamp(rightInset_, 0, width_ - 1 - left);
    const int usableH = height_ - top - bottom;
    const int usableW = width_ - left - right;
    if (usableH <= 0 || usableW <= 0) {
        // Degenerate chrome — fall back to the full surface rather than
        // drawing nothing (never confuse empty selection/zero area with a
        // rendering failure).
        top = 0;
        bottom = 0;
        left = 0;
        right = 0;
    }
    usableTop_ = top;
    usableHeight_ = height_ - top - bottom;
    usableLeft_ = left;
    usableWidth_ = width_ - left - right;
    aspect_ = static_cast<float>(usableWidth_) / static_cast<float>(usableHeight_);
    glViewport(usableLeft_, bottom, usableWidth_, usableHeight_);
}

void Renderer::resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    width_ = width;
    height_ = height;
    applyViewport();
}

void Renderer::setViewportInsets(int topPx, int bottomPx) {
    setViewportInsetsRect(topPx, bottomPx, leftInset_, rightInset_);
}

void Renderer::setViewportInsetsRect(int topPx, int bottomPx, int leftPx, int rightPx) {
    if (!std::isfinite(static_cast<float>(topPx)) ||
        !std::isfinite(static_cast<float>(bottomPx)) ||
        !std::isfinite(static_cast<float>(leftPx)) ||
        !std::isfinite(static_cast<float>(rightPx))) {
        return;
    }
    topInset_ = std::max(0, topPx);
    bottomInset_ = std::max(0, bottomPx);
    leftInset_ = std::max(0, leftPx);
    rightInset_ = std::max(0, rightPx);
    applyViewport();
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
        // F4-LIMITS (P3): read-only objects draw from the shared RenderMesh
        // (uFlatDeriv=1); the edit-mode overlays never exist for them.
        const bool isRead = !obj.editable;
        if (isRead) {
            if (!obj.readMesh || obj.readMesh->empty()) continue;
        } else if (obj.mesh.empty()) {
            continue;
        }
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
        glUniform1f(fillUseVColLoc_, 1.0f); // Fase 3: mesh albedo from vertex colors
        glUniform1f(fillFlatDerivLoc_, isRead ? 1.0f : 0.0f);
        glBindVertexArray(g.vao);
        // Z-fighting fix: the SOLID pass is pushed slightly back in depth
        // (GL_POLYGON_OFFSET_FILL, GLES3-compatible) so the wireframe and the
        // editable-vertex points rasterized at the exact surface depth win the
        // depth test cleanly — no stippled/dashed edges. States are restored
        // after the pass so nothing leaks into the overlay passes.
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0f, 1.0f);
        glDrawElements(GL_TRIANGLES, g.indexCount, g.indexType, nullptr);
        glDisable(GL_POLYGON_OFFSET_FILL);

        const bool active = (static_cast<int>(i) == scene.active);
        if (scene.editMode && active && !isRead) {
            // Wireframe of the active object (depth ON, native depth — clean
            // against the offset solid pass above).
            glUseProgram(lineProgram_);
            glUniformMatrix4fv(lineMvpLoc_, 1, GL_FALSE, mvp.m);
            glUniform1f(lineUseOverrideLoc_, 0.0f);
            glBindVertexArray(g.wireVao);
            glDrawArrays(GL_LINES, 0, g.wireCount);

            // Selected faces tinted: pulled towards the camera with polygon
            // offset and drawn WITHOUT depth writes so the edge/point
            // highlights drawn after them keep passing the depth test
            // against the (offset-back) solid surface.
            if (g.selFaceIndexCount > 0) {
                glDepthMask(GL_FALSE);
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(-1.0f, -1.0f);
                glUseProgram(fillProgram_);
                glUniformMatrix4fv(fillMvpLoc_, 1, GL_FALSE, mvp.m);
                glUniform3f(fillTintLoc_, 1.0f, 0.72f, 0.20f);
                glUniform1f(fillTintMixLoc_, 0.55f);
                glUniform1f(fillUseVColLoc_, 0.0f); // overlay: base color path
                glBindVertexArray(g.selFaceVao);
                glDrawElements(GL_TRIANGLES, g.selFaceIndexCount, GL_UNSIGNED_INT,
                               nullptr);
                glDisable(GL_POLYGON_OFFSET_FILL);
                glDepthMask(GL_TRUE);
            }
            // Selected edges + face-boundary highlight — depth test ON at
            // native depth: they win against the offset solid (no z-fighting)
            // and stay correctly OCCLUDED by foreground geometry.
            if (g.selEdgeCount > 0) {
                glUseProgram(lineProgram_);
                glUniformMatrix4fv(lineMvpLoc_, 1, GL_FALSE, mvp.m);
                glUniform1f(lineUseOverrideLoc_, 0.0f);
                glBindVertexArray(g.selEdgeVao);
                glDrawArrays(GL_LINES, 0, g.selEdgeCount);
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
                glUseProgram(pointProgram_);
                glUniformMatrix4fv(pointMvpLoc_, 1, GL_FALSE, mvp.m);
                glUniform4f(pointColorLoc_, 1.0f, 0.72f, 0.20f, 1.0f);
                glUniform1f(pointSizeLoc_, pointRadiusPx);
                glBindVertexArray(g.selPtVao);
                glDrawArrays(GL_POINTS, 0, g.selPtCount);
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

    // Gizmo overlay — active object, in BOTH modes (interface reorganization:
    // DECISÃO C — whole-object gizmo in object mode):
    //   - OBJECT mode: shown when an active object with geometry exists
    //     (editable mesh OR read-only RenderMesh — F4-LIMITS P3);
    //   - EDIT mode: ONLY when the selection is VALID (at least one alive
    //     selected element). An empty or invalidated selection draws NO
    //     gizmo, NO markers, NO highlights — the UI state matches what the
    //     renderer actually shows (stability correction).
    //   - GizmoTool::None ("Seleção") draws no gizmo and accepts no hit.
    const bool objectModeGizmo = [&] {
        const SceneObject* obj = scene.activeObject();
        if (!obj || scene.editMode) return false;
        return obj->editable ? !obj->mesh.empty()
                             : (obj->readMesh && !obj->readMesh->empty());
    }();
    const bool editModeGizmo = scene.editMode && sceneops::hasValidSelection(scene);
    if (gizmoTool != GizmoTool::None && (objectModeGizmo || editModeGizmo)) {
        drawGizmo(scene, vp, gizmoTool);
    }

    // Brush overlay (Fase 3): billboard circle at the last surface hit with
    // the REAL brush radius in world units. Depth-independent so the ring
    // stays readable while aiming. Set by the JNI stroke functions.
    if (brushOverlayActive_ && brushRadiusWorld_ > 0.0f) {
        drawBrushRing();
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
    const int usableW = usableWidth_ > 0 ? usableWidth_ : width_;
    const int usableH = usableHeight_ > 0 ? usableHeight_ : height_;
    sx = static_cast<float>(usableLeft_) +
         (ndcX * 0.5f + 0.5f) * static_cast<float>(usableW);
    sy = static_cast<float>(usableTop_) +
         (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(usableH);
    return true;
}

namespace {

// The gizmo pivot rule is SHARED with the JNI mapping (core/scene/GizmoPivot.h)
// so drawing and hit-testing can never diverge.

} // namespace

void Renderer::drawGizmo(const Scene& scene, const Mat4& vp, GizmoTool tool) {
    int objIndex = -1;
    Vec3 pivot{};
    if (!gizmopivot::pivotFor(scene, scene.editMode, pivot, objIndex)) return;

    const Vec3 toPivot = pivot - eye_;
    const float distPivot = std::max(length(toPivot), 0.5f);
    const int usableH = usableHeight_ > 0 ? usableHeight_ : height_;
    const float wpp = 2.0f * distPivot *
                      std::tan(cam_.fovYDeg * static_cast<float>(M_PI) / 180.0f * 0.5f) /
                      static_cast<float>(std::max(usableH, 1));
    // Visual scale: moderate growth with density (capped); hit targets use
    // the full density scale (see kHit* constants).
    const float visual = std::min(gizmoTouchScale_, 1.6f);
    const float axisLen = wpp * kGizmoArrowPx * visual;
    const float ringR = wpp * kGizmoRingPx * visual;

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

    // Handle dots: one per axis tip (axis color) — readable touch targets
    // (size scales with density; DECISÃO D).
    if (tool != GizmoTool::Rotate) {
        glDisable(GL_DEPTH_TEST);
        glBindVertexArray(gizmoPtVao_);
        glBindBuffer(GL_ARRAY_BUFFER, gizmoPtVbo_);
        glUseProgram(pointProgram_);
        glUniformMatrix4fv(pointMvpLoc_, 1, GL_FALSE, vp.m);
        glUniform1f(pointSizeLoc_, std::min(20.0f, 12.0f * gizmoTouchScale_));
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
        // Visible CENTER dot at the pivot (DECISÃO D: "gizmo de mover com
        // eixos identificáveis e centro visível") — white, no hit.
        gizmoPtScratch_.clear();
        gizmoPtScratch_.push_back(pivot.x);
        gizmoPtScratch_.push_back(pivot.y);
        gizmoPtScratch_.push_back(pivot.z);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(3 * sizeof(float)),
                     gizmoPtScratch_.data(), GL_DYNAMIC_DRAW);
        glUniform4f(pointColorLoc_, 1.0f, 1.0f, 1.0f, 1.0f);
        glUniform1f(pointSizeLoc_,
                    std::min(16.0f, 9.0f * gizmoTouchScale_));
        glDrawArrays(GL_POINTS, 0, 1);
        glBindVertexArray(0);
        glEnable(GL_DEPTH_TEST);
    }
}

Renderer::GizmoHit Renderer::gizmoHitAt(float xPx, float yPx, const Scene& scene,
                                        GizmoTool tool) const {
    GizmoHit out;
    // EXACTLY the same gate as the drawing (render()): object mode gizmo for
    // the active object (editable OR read-only — F4-LIMITS P3), edit-mode
    // gizmo only with a VALID selection, and GizmoTool::None never hits —
    // no invisible/ghost handles.
    const bool objectModeGizmo = [&] {
        const SceneObject* obj = scene.activeObject();
        if (!obj || scene.editMode) return false;
        return obj->editable ? !obj->mesh.empty()
                             : (obj->readMesh && !obj->readMesh->empty());
    }();
    const bool editModeGizmo = scene.editMode && sceneops::hasValidSelection(scene);
    if (tool == GizmoTool::None || !(objectModeGizmo || editModeGizmo)) return out;

    int objIndex = -1;
    Vec3 pivot{};
    if (!gizmopivot::pivotFor(scene, scene.editMode, pivot, objIndex)) return out;
    if (objIndex < 0) return out;

    float psx = 0.0f, psy = 0.0f;
    if (!projectToScreenPx(pivot, psx, psy)) return out;

    const Vec3 toPivot = pivot - eye_;
    const float distPivot = std::max(length(toPivot), 0.5f);
    const int usableH = usableHeight_ > 0 ? usableHeight_ : height_;
    const float wpp = 2.0f * distPivot *
                      std::tan(cam_.fovYDeg * static_cast<float>(M_PI) / 180.0f * 0.5f) /
                      static_cast<float>(std::max(usableH, 1));
    const float visual = std::min(gizmoTouchScale_, 1.6f);
    const float axisLen = wpp * kGizmoArrowPx * visual;
    const float ringR = wpp * kGizmoRingPx * visual;
    // Hit radii scale 1:1 with density (44 dp rule — DECISÃO D).
    const float hitTip = kHitTipPx * gizmoTouchScale_;
    const float hitRing = kHitRingPx * gizmoTouchScale_;
    const float hitCenter = kHitCenterPx * gizmoTouchScale_;
    const float hitSegment = kHitAxisSegmentPx * gizmoTouchScale_;

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
            if (std::sqrt(dx * dx + dy * dy) <= hitTip) {
                consider(sx, sy, a + 1, a, 1);
            }
            // Axis BODY: closest point on the pivot->tip screen segment so a
            // touch anywhere along the arrow grabs that axis (hit target far
            // larger than the 1 px visual line — DECISÃO D).
            float bx = 0.0f, by = 0.0f;
            if (!projectToScreenPx(pivot, bx, by)) continue;
            const float ex = sx - bx, ey = sy - by;
            const float len2 = ex * ex + ey * ey;
            float t = 0.0f;
            if (len2 > 1e-9f) {
                t = ((xPx - bx) * ex + (yPx - by) * ey) / len2;
                t = std::clamp(t, 0.0f, 1.0f);
            }
            const float cxp = bx + ex * t, cyp = by + ey * t;
            const float ddx = cxp - xPx, ddy = cyp - yPx;
            if (std::sqrt(ddx * ddx + ddy * ddy) <= hitSegment) {
                consider(cxp, cyp, a + 1, a, 1);
            }
        }
        if (tool == GizmoTool::Scale) {
            const float dx = psx - xPx, dy = psy - yPx;
            if (std::sqrt(dx * dx + dy * dy) <= hitCenter) {
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
                if (d <= hitRing && d < best) {
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

bool Renderer::buildPickRay(float xPx, float yPx, Vec3& dirOut) const {
    if (width_ <= 0 || height_ <= 0) return false;
    // SAME usable viewport rect the renderer drew with — taps map through the
    // exact projection used on screen (model centered in the visible area).
    const int usableW = usableWidth_ > 0 ? usableWidth_ : width_;
    const int usableH = usableHeight_ > 0 ? usableHeight_ : height_;
    const float ndcX =
            (2.0f * (xPx - static_cast<float>(usableLeft_)) / static_cast<float>(usableW)) - 1.0f;
    const float ndcY =
            1.0f - (2.0f * (yPx - static_cast<float>(usableTop_)) /
                    static_cast<float>(usableH));
    const float tanHalf = std::tan(cam_.fovYDeg * static_cast<float>(M_PI) / 180.0f * 0.5f);
    dirOut = normalize(cam_.forward() +
                       cam_.right() * (ndcX * tanHalf * aspect_) +
                       cam_.up() * (ndcY * tanHalf));
    return true;
}

Renderer::PickHit Renderer::pickAtPixel(float xPx, float yPx, const Scene& scene,
                                        float snapRadiusPx) const {
    PickHit out;
    Vec3 dir{};
    if (!buildPickRay(xPx, yPx, dir)) return out;

    // 1) Nearest face hit across all objects — SAME triangulation the
    //    renderer draws (renderdata::faceTriangles), so what you tap is what
    //    you see (single geometric source).
    float bestT = std::numeric_limits<float>::max();
    int bestObj = -1;
    ElemId bestFace = kNull;
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        const SceneObject& obj = scene.objects[i];
        // F4-LIMITS (P3): read-only objects participate in OBJECT-mode
        // picking (activation + move gizmo need them) via their index
        // triples; edit mode ignores them (no editable elements).
        if (!obj.editable) {
            if (scene.editMode) continue;
            if (!obj.readMesh || obj.readMesh->empty()) continue;
            const Vec3 local = Vec3{eye_.x - obj.position.x, eye_.y - obj.position.y,
                                    eye_.z - obj.position.z};
            const auto& idx = obj.readMesh->indices;
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                const float tt = rayTriangle(
                        local, dir,
                        obj.readMesh->positions[idx[t + 0]],
                        obj.readMesh->positions[idx[t + 1]],
                        obj.readMesh->positions[idx[t + 2]]);
                if (tt > 0.0f && tt < bestT) {
                    bestT = tt;
                    bestObj = static_cast<int>(i);
                    bestFace = kNull; // no face id space on read meshes
                }
            }
            continue;
        }
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

// ------------------------------------------------------------------- brush --

bool Renderer::surfaceHitAt(float xPx, float yPx, const Scene& scene,
                            SurfaceHit& out) const {
    out = SurfaceHit{};
    Vec3 dir{};
    if (!buildPickRay(xPx, yPx, dir)) return false;

    // Nearest face across all objects — the SAME triangulation the renderer
    // draws (renderdata::faceTriangles): the brush hits exactly what is on
    // screen. No snapping, no selection side effects.
    // The triangle id buffer is reused across faces/objects (cleared before
    // each call — faceTriangles() appends, it never clears): this raycast
    // runs on EVERY touch-move event, so no allocation may depend on the
    // face count (Fase 5: evitar alocações por frame).
    float bestT = std::numeric_limits<float>::max();
    int bestObj = -1;
    ElemId bestFace = kNull;
    std::vector<ElemId> tris;
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        const SceneObject& obj = scene.objects[i];
        // F4-LIMITS (P3): the brush NEVER targets read-only objects (they
        // accept no sculpt/paint) — skipped entirely here.
        if (!obj.editable) continue;
        if (obj.mesh.empty()) continue;
        const Vec3 local = Vec3{eye_.x - obj.position.x, eye_.y - obj.position.y,
                                eye_.z - obj.position.z};
        for (const ElemId f : obj.mesh.aliveFaceIds()) {
            tris.clear();
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
    if (bestObj < 0) return false;
    if (!std::isfinite(bestT)) return false;

    const SceneObject& obj = scene.objects[static_cast<size_t>(bestObj)];
    out.hit = true;
    out.objectIndex = bestObj;
    out.faceId = bestFace;
    out.world = eye_ + dir * bestT;
    out.local = Vec3{out.world.x - obj.position.x, out.world.y - obj.position.y,
                     out.world.z - obj.position.z};
    return blone::ops::isFiniteVec(out.local.x, out.local.y, out.local.z);
}

void Renderer::setBrushOverlay(bool active, const Vec3& world, float radiusWorld,
                               float r, float g, float b, float a) {
    brushOverlayActive_ = active;
    brushOverlayWorld_ = world;
    brushRadiusWorld_ = std::isfinite(radiusWorld) ? std::max(0.0f, radiusWorld) : 0.0f;
    brushRingRgba_[0] = std::isfinite(r) ? r : 1.0f;
    brushRingRgba_[1] = std::isfinite(g) ? g : 0.72f;
    brushRingRgba_[2] = std::isfinite(b) ? b : 0.20f;
    brushRingRgba_[3] = std::isfinite(a) ? a : 0.9f;
}

void Renderer::drawBrushRing() {
    if (lineProgram_ == 0) return;
    // Billboard circle in the camera plane at the last hit point: constant
    // apparent radius regardless of the surface orientation, so it reads as
    // the REAL brush extent (world units, like the deformation itself).
    Vec3 u = cam_.right();
    Vec3 v = cam_.up();
    if (length(u) < 0.5f || length(v) < 0.5f) return; // degenerate camera

    gizmoLineScratch_.clear();
    constexpr int kSegs = 40;
    Vec3 prev = brushOverlayWorld_ + u * brushRadiusWorld_;
    for (int s = 1; s <= kSegs; ++s) {
        const float ang = static_cast<float>(M_PI) * 2.0f *
                          static_cast<float>(s) / static_cast<float>(kSegs);
        const Vec3 cur = brushOverlayWorld_ +
                         (u * (brushRadiusWorld_ * std::cos(ang)) +
                          v * (brushRadiusWorld_ * std::sin(ang)));
        gizmoLineScratch_.push_back(
                {prev.x, prev.y, prev.z, brushRingRgba_[0], brushRingRgba_[1],
                 brushRingRgba_[2], brushRingRgba_[3]});
        gizmoLineScratch_.push_back(
                {cur.x, cur.y, cur.z, brushRingRgba_[0], brushRingRgba_[1],
                 brushRingRgba_[2], brushRingRgba_[3]});
        prev = cur;
    }
    // A center dot marks the exact application point.
    const float cx = brushOverlayWorld_.x, cy = brushOverlayWorld_.y,
                cz = brushOverlayWorld_.z;
    gizmoLineScratch_.push_back(
            {cx - 0.02f * length(u), cy, cz, brushRingRgba_[0], brushRingRgba_[1],
             brushRingRgba_[2], brushRingRgba_[3]});
    gizmoLineScratch_.push_back(
            {cx + 0.02f * length(u), cy, cz, brushRingRgba_[0], brushRingRgba_[1],
             brushRingRgba_[2], brushRingRgba_[3]});

    glDisable(GL_DEPTH_TEST);
    glUseProgram(lineProgram_);
    const Mat4 vp = proj_ * view_;
    glUniformMatrix4fv(lineMvpLoc_, 1, GL_FALSE, vp.m);
    glUniform1f(lineUseOverrideLoc_, 0.0f);
    glBindVertexArray(gizmoLineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, gizmoLineVbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(gizmoLineScratch_.size() * sizeof(LineVertex)),
                 gizmoLineScratch_.data(), GL_DYNAMIC_DRAW);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(gizmoLineScratch_.size()));
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

} // namespace blone
