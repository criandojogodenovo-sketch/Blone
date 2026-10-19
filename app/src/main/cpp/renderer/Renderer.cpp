#include "renderer/Renderer.h"

#include <android/log.h>

#include <cmath>
#include <vector>

namespace blone {

namespace {

const char* kTag = "BloneNative";

const char* kCubeVertexShader = R"(#version 300 es
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

const char* kCubeFragmentShader = R"(#version 300 es
precision mediump float;
in vec3 vNormal;
in vec3 vWorldPos;
uniform vec3 uLightDir;
uniform vec3 uEyePos;
uniform float uSelected;
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
    if (uSelected > 0.5) {
        color = mix(color, vec3(1.0, 0.72, 0.20), 0.25);
    }
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

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Shader compile failed: %s", log);
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
        __android_log_print(ANDROID_LOG_ERROR, kTag, "Program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

// 12 edges of an axis-aligned box, as line vertices with a placeholder color.
std::vector<LineVertex> boxEdges(const Aabb& box) {
    const float x0 = box.minX, y0 = box.minY, z0 = box.minZ;
    const float x1 = box.maxX, y1 = box.maxY, z1 = box.maxZ;
    const float c[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // overridden in shader

    const float p[8][3] = {
            {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1},
            {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1},
    };
    const int edges[12][2] = {
            {0, 1}, {1, 2}, {2, 3}, {3, 0}, // bottom
            {4, 5}, {5, 6}, {6, 7}, {7, 4}, // top
            {0, 4}, {1, 5}, {2, 6}, {3, 7}, // verticals
    };

    std::vector<LineVertex> out;
    out.reserve(24);
    for (const auto& e : edges) {
        out.push_back({p[e[0]][0], p[e[0]][1], p[e[0]][2], c[0], c[1], c[2], c[3]});
        out.push_back({p[e[1]][0], p[e[1]][1], p[e[1]][2], c[0], c[1], c[2], c[3]});
    }
    return out;
}

} // namespace

void Renderer::init() {
    // Release any GL objects from a previous context before rebuilding.
    if (cubeProgram_ != 0) glDeleteProgram(cubeProgram_);
    if (lineProgram_ != 0) glDeleteProgram(lineProgram_);
    if (cubeVao_ != 0) glDeleteVertexArrays(1, &cubeVao_);
    if (cubeVbo_ != 0) glDeleteBuffers(1, &cubeVbo_);
    if (cubeEbo_ != 0) glDeleteBuffers(1, &cubeEbo_);
    if (sceneVao_ != 0) glDeleteVertexArrays(1, &sceneVao_);
    if (sceneVbo_ != 0) glDeleteBuffers(1, &sceneVbo_);
    if (edgeVao_ != 0) glDeleteVertexArrays(1, &edgeVao_);
    if (edgeVbo_ != 0) glDeleteBuffers(1, &edgeVbo_);
    cubeProgram_ = 0; lineProgram_ = 0;
    cubeVao_ = 0; cubeVbo_ = 0; cubeEbo_ = 0;
    sceneVao_ = 0; sceneVbo_ = 0;
    edgeVao_ = 0; edgeVbo_ = 0;

    buildPrograms();
    buildCubeBuffers();
    buildLineBuffers();
    buildEdgeBuffer();

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glClearColor(0.055f, 0.071f, 0.094f, 1.0f); // #0E1218 — matches the app chrome
}

void Renderer::buildPrograms() {
    cubeProgram_ = linkProgram(kCubeVertexShader, kCubeFragmentShader);
    lineProgram_ = linkProgram(kLineVertexShader, kLineFragmentShader);

    if (cubeProgram_ != 0) {
        cubeMvpLoc_ = glGetUniformLocation(cubeProgram_, "uMVP");
        cubeModelLoc_ = glGetUniformLocation(cubeProgram_, "uModel");
        cubeLightDirLoc_ = glGetUniformLocation(cubeProgram_, "uLightDir");
        cubeEyePosLoc_ = glGetUniformLocation(cubeProgram_, "uEyePos");
        cubeSelectedLoc_ = glGetUniformLocation(cubeProgram_, "uSelected");
    }
    if (lineProgram_ != 0) {
        lineMvpLoc_ = glGetUniformLocation(lineProgram_, "uMVP");
        lineOverrideLoc_ = glGetUniformLocation(lineProgram_, "uOverride");
        lineUseOverrideLoc_ = glGetUniformLocation(lineProgram_, "uUseOverride");
    }
}

void Renderer::buildCubeBuffers() {
    cube_ = buildCube(0.0f, 1.0f, 0.0f, 1.0f); // rests on the grid (y = 0)
    cubeBounds_ = cube_.bounds;
    cubeIndexCount_ = static_cast<GLsizei>(cube_.indices.size());

    glGenVertexArrays(1, &cubeVao_);
    glGenBuffers(1, &cubeVbo_);
    glGenBuffers(1, &cubeEbo_);

    glBindVertexArray(cubeVao_);
    glBindBuffer(GL_ARRAY_BUFFER, cubeVbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(cube_.vertices.size() * sizeof(float)),
                 cube_.vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cubeEbo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(cube_.indices.size() * sizeof(uint16_t)),
                 cube_.indices.data(), GL_STATIC_DRAW);

    const GLsizei stride = 6 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void Renderer::buildLineBuffers() {
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

void Renderer::buildEdgeBuffer() {
    std::vector<LineVertex> edges = boxEdges(cubeBounds_);
    edgeVertexCount_ = static_cast<GLsizei>(edges.size());

    glGenVertexArrays(1, &edgeVao_);
    glGenBuffers(1, &edgeVbo_);
    glBindVertexArray(edgeVao_);
    glBindBuffer(GL_ARRAY_BUFFER, edgeVbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(edges.size() * sizeof(LineVertex)),
                 edges.data(), GL_STATIC_DRAW);

    const GLsizei stride = sizeof(LineVertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

void Renderer::resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    width_ = width;
    height_ = height;
    aspect_ = static_cast<float>(width) / static_cast<float>(height);
    glViewport(0, 0, width, height);
}

void Renderer::render(const OrbitCamera& camera, bool selected) {
    if (cubeProgram_ == 0 || lineProgram_ == 0) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        return;
    }

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const Mat4 proj = Mat4::perspective(camera.fovYDeg, aspect_, 0.1f, 200.0f);
    const Mat4 view = camera.viewMatrix();
    const Mat4 mvp = proj * view;               // model is identity (geometry pre-placed)
    const Mat4 model = Mat4::identity();

    // Camera state snapshot for picking (matches what is drawn this frame).
    eye_ = camera.eye();
    forward_ = camera.forward();
    right_ = camera.right();
    up_ = camera.up();
    fovYDeg_ = camera.fovYDeg;

    // --- Cube ---
    glUseProgram(cubeProgram_);
    glUniformMatrix4fv(cubeMvpLoc_, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(cubeModelLoc_, 1, GL_FALSE, model.m);
    const Vec3 lightDir = normalize(Vec3{0.45f, 0.85f, 0.35f});
    glUniform3f(cubeLightDirLoc_, lightDir.x, lightDir.y, lightDir.z);
    glUniform3f(cubeEyePosLoc_, eye_.x, eye_.y, eye_.z);
    glUniform1f(cubeSelectedLoc_, selected ? 1.0f : 0.0f);
    glBindVertexArray(cubeVao_);
    glDrawElements(GL_TRIANGLES, cubeIndexCount_, GL_UNSIGNED_SHORT, nullptr);

    // --- Grid + axes (alpha-blended lines) ---
    glDepthMask(GL_FALSE);
    glUseProgram(lineProgram_);
    glUniformMatrix4fv(lineMvpLoc_, 1, GL_FALSE, mvp.m);
    glUniform4f(lineOverrideLoc_, 1.0f, 1.0f, 1.0f, 1.0f);
    glUniform1f(lineUseOverrideLoc_, 0.0f);
    glBindVertexArray(sceneVao_);
    glDrawArrays(GL_LINES, 0, sceneVertexCount_);
    glDepthMask(GL_TRUE);

    // --- Selection outline ---
    if (selected) {
        glUseProgram(lineProgram_);
        glUniformMatrix4fv(lineMvpLoc_, 1, GL_FALSE, mvp.m);
        glUniform4f(lineOverrideLoc_, 1.0f, 0.72f, 0.20f, 1.0f);
        glUniform1f(lineUseOverrideLoc_, 1.0f);
        glBindVertexArray(edgeVao_);
        glDrawArrays(GL_LINES, 0, edgeVertexCount_);
        glUniform1f(lineUseOverrideLoc_, 0.0f);
    }

    glBindVertexArray(0);
}

bool Renderer::pickAtPixel(float xPx, float yPx) const {
    if (width_ <= 0 || height_ <= 0) return false;

    const float ndcX = (2.0f * xPx / static_cast<float>(width_)) - 1.0f;
    const float ndcY = 1.0f - (2.0f * yPx / static_cast<float>(height_));

    const float tanHalf = std::tan(fovYDeg_ * static_cast<float>(M_PI) / 180.0f * 0.5f);
    const Vec3 rayDir = normalize(forward_ + right_ * (ndcX * tanHalf * aspect_) +
                                  up_ * (ndcY * tanHalf));

    constexpr float kMaxPickDistance = 120.0f;
    return rayIntersectsAabb(eye_, rayDir, cubeBounds_, kMaxPickDistance);
}

} // namespace blone
