#include "Renderer.h"

#include "../core/Log.h"
#include "../core/Scene.h"
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
    float ambient = 0.28;
    vec3 viewDir = normalize(uEyePos - vWorldPos);
    vec3 halfV = normalize(l + viewDir);
    float spec = pow(max(dot(n, halfV), 0.0), 32.0) * 0.35;
    vec3 color = base * (ambient + diff * 0.78) + vec3(spec);
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

// Fan triangulation of a polygon loop (loops from our primitives and
// operations are convex or near-convex; documented Phase 2 policy).
void triangulateFan(const std::vector<ElemId>& loop,
                    std::vector<GLuint>& indices) {
    for (size_t i = 1; i + 1 < loop.size(); ++i) {
        indices.push_back(static_cast<GLuint>(loop[0]));
        indices.push_back(static_cast<GLuint>(loop[i]));
        indices.push_back(static_cast<GLuint>(loop[i + 1]));
    }
}

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

constexpr float kMaxPickDistance = 200.0f;

} // namespace

void Renderer::init() {
    if (fillProgram_ != 0) glDeleteProgram(fillProgram_);
    if (lineProgram_ != 0) glDeleteProgram(lineProgram_);
    if (pointProgram_ != 0) glDeleteProgram(pointProgram_);
    if (sceneVao_ != 0) glDeleteVertexArrays(1, &sceneVao_);
    if (sceneVbo_ != 0) glDeleteBuffers(1, &sceneVbo_);
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

    std::vector<FillVertex> verts;
    std::vector<GLuint> indices;
    std::vector<LineVertex> wire;

    const size_t faceGuess = obj.mesh.aliveFaceCount();
    verts.reserve(faceGuess * 4);
    indices.reserve(faceGuess * 6);
    wire.reserve(obj.mesh.aliveEdgeCount() * 2);

    for (const ElemId f : obj.mesh.aliveFaceIds()) {
        const std::vector<ElemId> loop = obj.mesh.faceVertices(f);
        if (loop.size() < 3) continue;
        const GLuint base = static_cast<GLuint>(verts.size());
        const Vec3 n = obj.mesh.faceNormal(f);
        for (const ElemId v : loop) {
            const Vec3& p = obj.mesh.vrt(v).pos;
            verts.push_back({p.x, p.y, p.z, n.x, n.y, n.z});
        }
        std::vector<GLuint> local;
        local.reserve((loop.size() - 2) * 3);
        triangulateFan(loop, local);
        for (const GLuint idx : local) indices.push_back(base + idx);
    }
    for (const auto& e : obj.mesh.edges()) {
        if (!e.alive) continue;
        const ElemId h = e.he;
        const Vec3& a = obj.mesh.vrt(obj.mesh.heg(h).origin).pos;
        const Vec3& b = obj.mesh.vrt(obj.mesh.heg(obj.mesh.heg(h).twin).origin).pos;
        wire.push_back({a.x, a.y, a.z, 0.08f, 0.09f, 0.11f, 0.9f});
        wire.push_back({b.x, b.y, b.z, 0.08f, 0.09f, 0.11f, 0.9f});
    }
    g.bounds = obj.mesh.computeBounds();

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
                 static_cast<GLsizeiptr>(indices.size() * sizeof(GLuint)),
                 indices.data(), GL_STATIC_DRAW);
    const GLsizei stride = sizeof(FillVertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    g.indexCount = static_cast<GLsizei>(indices.size());

    // Wireframe.
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

    // Vertex points (true mesh vertices).
    std::vector<float> pts;
    pts.reserve(obj.mesh.aliveVertexCount() * 3);
    for (const auto& v : obj.mesh.vertices()) {
        if (!v.alive) continue;
        pts.push_back(v.pos.x);
        pts.push_back(v.pos.y);
        pts.push_back(v.pos.z);
    }
    glGenVertexArrays(1, &g.ptVao);
    glGenBuffers(1, &g.ptVbo);
    glBindVertexArray(g.ptVao);
    glBindBuffer(GL_ARRAY_BUFFER, g.ptVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(pts.size() * sizeof(float)),
                 pts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    g.ptCount = static_cast<GLsizei>(pts.size() / 3);
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

    // Faces: duplicated flat vertices per selected face.
    std::vector<FillVertex> fv;
    std::vector<GLuint> fi;
    for (const ElemId f : obj.sel.faces) {
        if (!obj.mesh.faceAlive(f)) continue;
        const std::vector<ElemId> loop = obj.mesh.faceVertices(f);
        if (loop.size() < 3) continue;
        const GLuint base = static_cast<GLuint>(fv.size());
        const Vec3 n = obj.mesh.faceNormal(f);
        for (const ElemId v : loop) {
            const Vec3& p = obj.mesh.vrt(v).pos;
            fv.push_back({p.x, p.y, p.z, n.x, n.y, n.z});
        }
        std::vector<GLuint> local;
        local.reserve((loop.size() - 2) * 3);
        triangulateFan(loop, local);
        for (const GLuint idx : local) fi.push_back(base + idx);
    }

    if (!fi.empty()) {
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
                     static_cast<GLsizeiptr>(fi.size() * sizeof(GLuint)),
                     fi.data(), GL_STATIC_DRAW);
        const GLsizei stride = sizeof(FillVertex);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);
        g.selFaceIndexCount = static_cast<GLsizei>(fi.size());
    }

    // Edges.
    std::vector<LineVertex> el;
    for (const ElemId e : obj.sel.edges) {
        if (!obj.mesh.edgeAlive(e)) continue;
        const ElemId h = obj.mesh.edg(e).he;
        const Vec3& a = obj.mesh.vrt(obj.mesh.heg(h).origin).pos;
        const Vec3& b = obj.mesh.vrt(obj.mesh.heg(obj.mesh.heg(h).twin).origin).pos;
        el.push_back({a.x, a.y, a.z, 1.0f, 0.72f, 0.20f, 1.0f});
        el.push_back({b.x, b.y, b.z, 1.0f, 0.72f, 0.20f, 1.0f});
    }
    // Face-selection boundary edges (so selected faces read clearly).
    for (const ElemId f : obj.sel.faces) {
        if (!obj.mesh.faceAlive(f)) continue;
        for (const ElemId e : obj.mesh.faceEdges(f)) {
            const ElemId h = obj.mesh.edg(e).he;
            const Vec3& a = obj.mesh.vrt(obj.mesh.heg(h).origin).pos;
            const Vec3& b = obj.mesh.vrt(obj.mesh.heg(obj.mesh.heg(h).twin).origin).pos;
            el.push_back({a.x, a.y, a.z, 1.0f, 0.72f, 0.20f, 1.0f});
            el.push_back({b.x, b.y, b.z, 1.0f, 0.72f, 0.20f, 1.0f});
        }
    }
    if (!el.empty()) {
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

    // Selected vertices.
    std::vector<float> pv;
    for (const ElemId v : obj.sel.verts) {
        if (!obj.mesh.vertexAlive(v)) continue;
        const Vec3& p = obj.mesh.vrt(v).pos;
        pv.push_back(p.x);
        pv.push_back(p.y);
        pv.push_back(p.z);
    }
    if (!pv.empty()) {
        glGenVertexArrays(1, &g.selPtVao);
        glGenBuffers(1, &g.selPtVbo);
        glBindVertexArray(g.selPtVao);
        glBindBuffer(GL_ARRAY_BUFFER, g.selPtVbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(pv.size() * sizeof(float)),
                     pv.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                              reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
        g.selPtCount = static_cast<GLsizei>(pv.size() / 3);
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
                      float pointRadiusPx) {
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
            // Selected edges + face-boundary highlight.
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
            // Object mode: amber box outline around the active object.
            std::vector<LineVertex> box = boxEdgesLines(g.bounds);
            for (LineVertex& lv : box) {
                lv.x += obj.position.x;
                lv.y += obj.position.y;
                lv.z += obj.position.z;
                lv.r = 1.0f;
                lv.g = 0.72f;
                lv.b = 0.20f;
                lv.a = 1.0f;
            }
            GLuint vao, vbo;
            glGenVertexArrays(1, &vao);
            glGenBuffers(1, &vbo);
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(box.size() * sizeof(LineVertex)),
                         box.data(), GL_DYNAMIC_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                                  reinterpret_cast<void*>(0));
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                                  reinterpret_cast<void*>(3 * sizeof(float)));
            glEnableVertexAttribArray(1);
            glUseProgram(lineProgram_);
            glUniformMatrix4fv(lineMvpLoc_, 1, GL_FALSE, vp.m);
            glUniform1f(lineUseOverrideLoc_, 0.0f);
            glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(box.size()));
            glBindVertexArray(0);
            glDeleteBuffers(1, &vbo);
            glDeleteVertexArrays(1, &vao);
        }
    }
    glBindVertexArray(0);
}

Vec3 Renderer::projectToScreen(const Vec3& world) const {
    const Mat4 vp = proj_ * view_;
    const float cx = vp.m[0] * world.x + vp.m[4] * world.y + vp.m[8] * world.z + vp.m[12];
    const float cy = vp.m[1] * world.x + vp.m[5] * world.y + vp.m[9] * world.z + vp.m[13];
    const float cw = vp.m[3] * world.x + vp.m[7] * world.y + vp.m[11] * world.z + vp.m[15];
    const float ndcX = cx / cw;
    const float ndcY = cy / cw;
    return Vec3{(ndcX * 0.5f + 0.5f) * static_cast<float>(width_),
                (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(height_), 0.0f};
}

bool Renderer::screenSpaceDistance(const Vec3& world, float xPx, float yPx,
                                   float& distOut) const {
    const Vec3 s = projectToScreen(world);
    const float dx = s.x - xPx;
    const float dy = s.y - yPx;
    distOut = std::sqrt(dx * dx + dy * dy);
    return true;
}

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

    // 1) Nearest face hit across all objects (occlusion-respecting).
    float bestT = std::numeric_limits<float>::max();
    int bestObj = -1;
    ElemId bestFace = kNull;
    std::vector<ElemId> bestLoop;
    for (size_t i = 0; i < scene.objects.size(); ++i) {
        const SceneObject& obj = scene.objects[i];
        if (obj.mesh.empty()) continue;
        const Vec3 local = Vec3{eye_.x - obj.position.x, eye_.y - obj.position.y,
                                eye_.z - obj.position.z};
        for (const ElemId f : obj.mesh.aliveFaceIds()) {
            const std::vector<ElemId> loop = obj.mesh.faceVertices(f);
            if (loop.size() < 3) continue;
            const Vec3& v0 = obj.mesh.vrt(loop[0]).pos;
            for (size_t k = 1; k + 1 < loop.size(); ++k) {
                const float t = rayTriangle(local, dir, v0,
                                            obj.mesh.vrt(loop[k]).pos,
                                            obj.mesh.vrt(loop[k + 1]).pos);
                if (t > 0.0f && t < bestT) {
                    bestT = t;
                    bestObj = static_cast<int>(i);
                    bestFace = f;
                    bestLoop = loop;
                }
            }
        }
    }
    if (bestObj < 0) return out; // nothing visible under the finger

    const SceneObject& obj = scene.objects[static_cast<size_t>(bestObj)];

    // 2) Vertex/edge snapping around the hit, depth-checked (occlusion).
    if (scene.editMode) {
        float bestScreen = snapRadiusPx;
        ElemId bestElem = kNull;
        int bestKind = 0;
        if (scene.selectionMode == SelMode::Vertex) {
            for (const ElemId cand : bestLoop) {
                const Vec3 w = obj.mesh.vrt(cand).pos + obj.position;
                float d;
                if (!screenSpaceDistance(w, xPx, yPx, d)) continue;
                const Vec3 rel = w - eye_;
                const float depth = dot(rel, dir);
                if (depth > bestT + 0.25f) continue; // hidden behind surface
                if (d < bestScreen) {
                    bestScreen = d;
                    bestElem = cand;
                    bestKind = 1;
                }
            }
        } else if (scene.selectionMode == SelMode::Edge) {
            for (const ElemId e : obj.mesh.faceEdges(bestFace)) {
                const ElemId h = obj.mesh.edg(e).he;
                const Vec3 a = obj.mesh.vrt(obj.mesh.heg(h).origin).pos + obj.position;
                const Vec3 b = obj.mesh.vrt(obj.mesh.heg(obj.mesh.heg(h).twin).origin).pos +
                               obj.position;
                const Vec3 mid = (a + b) * 0.5f;
                float d;
                if (!screenSpaceDistance(mid, xPx, yPx, d)) continue;
                const Vec3 rel = mid - eye_;
                const float depth = dot(rel, dir);
                if (depth > bestT + 0.25f) continue;
                if (d < bestScreen) {
                    bestScreen = d;
                    bestElem = e;
                    bestKind = 2;
                }
            }
        }
        if (bestKind != 0) {
            out.hit = true;
            out.kind = bestKind;
            out.objectIndex = bestObj;
            out.elemId = bestElem;
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
