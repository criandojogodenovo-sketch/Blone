#pragma once
// Minimal column-major 4x4 matrix and 3D vector math for the Blone renderer.
// Deliberately tiny and dependency-free (no GLM) for the current needs.

#include <cmath>

namespace blone {

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

} // namespace blone
