#pragma once
// Minimal column-major 4x4 matrix and 3D vector math for the Blone renderer.
// Deliberately tiny and dependency-free (no GLM) for the current needs.

#include <cmath>

namespace blone {

// Axis-aligned bounding box (shared by picking, rendering and serialization).
struct Aabb {
    float minX = 0, minY = 0, minZ = 0;
    float maxX = 0, maxY = 0, maxZ = 0;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(const Vec3& a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

inline float length(const Vec3& a) { return std::sqrt(dot(a, a)); }

inline Vec3 normalize(const Vec3& a) {
    const float len = length(a);
    if (len <= 1e-8f) return {0.0f, 0.0f, 0.0f};
    const float inv = 1.0f / len;
    return {a.x * inv, a.y * inv, a.z * inv};
}

// Column-major 4x4 matrix, compatible with OpenGL's uniformMatrix4fv.
struct Mat4 {
    // m[col * 4 + row]
    float m[16] = {1, 0, 0, 0,
                   0, 1, 0, 0,
                   0, 0, 1, 0,
                   0, 0, 0, 1};

    static Mat4 identity() { return Mat4{}; }

    static Mat4 perspective(float fovYDeg, float aspect, float nearZ, float farZ) {
        Mat4 r{};
        const float rad = fovYDeg * static_cast<float>(M_PI) / 180.0f;
        const float tanHalf = std::tan(rad * 0.5f);
        const float f = 1.0f / tanHalf;
        for (float& v : r.m) v = 0.0f;
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (farZ + nearZ) / (nearZ - farZ);
        r.m[11] = -1.0f;
        r.m[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
        return r;
    }

    static Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
        const Vec3 f = normalize(center - eye);          // forward
        const Vec3 s = normalize(cross(f, up));          // right
        const Vec3 u = cross(s, f);                      // camera up

        Mat4 r{};
        r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;
        r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
        r.m[12] = -dot(s, eye);
        r.m[13] = -dot(u, eye);
        r.m[14] = dot(f, eye);
        r.m[3] = 0.0f; r.m[7] = 0.0f; r.m[11] = 0.0f; r.m[15] = 1.0f;
        return r;
    }

    static Mat4 translate(float tx, float ty, float tz) {
        Mat4 r = identity();
        r.m[12] = tx;
        r.m[13] = ty;
        r.m[14] = tz;
        return r;
    }

    static Mat4 scale(float s) {
        Mat4 r = identity();
        r.m[0] = s; r.m[5] = s; r.m[10] = s;
        return r;
    }

    // Rodrigues rotation around a unit axis, in degrees.
    static Mat4 rotationAxisDeg(const Vec3& axis, float deg) {
        const Vec3 a = normalize(axis);
        const float rad = deg * static_cast<float>(M_PI) / 180.0f;
        const float c = std::cos(rad), s = std::sin(rad), t = 1.0f - c;
        const float x = a.x, y = a.y, z = a.z;
        Mat4 r{};
        r.m[0] = t * x * x + c;     r.m[4] = t * x * y - s * z; r.m[8]  = t * x * z + s * y;
        r.m[1] = t * x * y + s * z; r.m[5] = t * y * y + c;     r.m[9]  = t * y * z - s * x;
        r.m[2] = t * x * z - s * y; r.m[6] = t * y * z + s * x; r.m[10] = t * z * z + c;
        return r;
    }

    static Mat4 rotationYDeg(float deg) {
        // Right-handed CCW around +Y (delegates to Rodrigues to keep one
        // single, tested implementation of the math).
        return rotationAxisDeg(Vec3{0.0f, 1.0f, 0.0f}, deg);
    }

    // Transforms a point (w = 1).
    Vec3 transformPoint(const Vec3& p) const {
        return Vec3{
            m[0] * p.x + m[4] * p.y + m[8]  * p.z + m[12],
            m[1] * p.x + m[5] * p.y + m[9]  * p.z + m[13],
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14],
        };
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = sum;
        }
    }
    return r;
}

// ---- frustum vs AABB (missão F4-LIMITS, P7: frustum culling) ---------------
//
// Gribb-Hartmann plane extraction from the column-major VIEW*PROJECTION
// matrix (OpenGL clip convention: clip.x = m[0]x + m[4]y + m[8]z + m[12],
// clip.w = m[3]x + m[7]y + m[11]z + m[15]). An AABB is VISIBLE when at
// least one of its 8 corners passes EVERY plane; if any plane rejects all
// 8 corners the box is entirely outside and the object can be culled.
// Pure math (no GL) so the host tests pin it; the renderer uses it to skip
// whole-object draw passes (overlays included) — the gizmo is independent.
inline bool aabbInFrustum(const Aabb& b, const Mat4& vp) {
    const float corners[8][3] = {
            {b.minX, b.minY, b.minZ}, {b.maxX, b.minY, b.minZ},
            {b.minX, b.maxY, b.minZ}, {b.maxX, b.maxY, b.minZ},
            {b.minX, b.minY, b.maxZ}, {b.maxX, b.minY, b.maxZ},
            {b.minX, b.maxY, b.maxZ}, {b.maxX, b.maxY, b.maxZ},
    };
    // The 6 planes (a, b, c, d) — each rejects the half-space outside it.
    const float planes[6][4] = {
            {vp.m[3] + vp.m[0], vp.m[7] + vp.m[4],
             vp.m[11] + vp.m[8], vp.m[15] + vp.m[12]},  // left
            {vp.m[3] - vp.m[0], vp.m[7] - vp.m[4],
             vp.m[11] - vp.m[8], vp.m[15] - vp.m[12]},  // right
            {vp.m[3] + vp.m[1], vp.m[7] + vp.m[5],
             vp.m[11] + vp.m[9], vp.m[15] + vp.m[13]},  // bottom
            {vp.m[3] - vp.m[1], vp.m[7] - vp.m[5],
             vp.m[11] - vp.m[9], vp.m[15] - vp.m[13]},  // top
            {vp.m[3] + vp.m[2], vp.m[7] + vp.m[6],
             vp.m[11] + vp.m[10], vp.m[15] + vp.m[14]}, // near
            {vp.m[3] - vp.m[2], vp.m[7] - vp.m[6],
             vp.m[11] - vp.m[10], vp.m[15] - vp.m[14]}, // far
    };
    for (const auto& pl : planes) {
        bool allOut = true;
        for (const auto& c : corners) {
            if (pl[0] * c[0] + pl[1] * c[1] + pl[2] * c[2] + pl[3] >= 0.0f) {
                allOut = false;
                break;
            }
        }
        if (allOut) return false; // entirely outside this plane
    }
    return true;
}

} // namespace blone
