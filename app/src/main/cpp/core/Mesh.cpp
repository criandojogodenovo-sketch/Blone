#include "core/Mesh.h"

namespace blone {

CubeMesh buildCube(float cx, float cy, float cz, float halfExtent) {
    // Face order: +X, -X, +Y, -Y, +Z, -Z. Each face has 4 vertices; each
    // vertex is a flat 6-float tuple: px, py, pz, nx, ny, nz.
    // clang-format off
    const float faces[6][4][6] = {
        // +X (normal 1,0,0)
        {{ 1, -1, -1,  1, 0, 0}, { 1,  1, -1,  1, 0, 0}, { 1,  1,  1,  1, 0, 0}, { 1, -1,  1,  1, 0, 0}},
        // -X (normal -1,0,0)
        {{-1, -1,  1, -1, 0, 0}, {-1,  1,  1, -1, 0, 0}, {-1,  1, -1, -1, 0, 0}, {-1, -1, -1, -1, 0, 0}},
        // +Y (normal 0,1,0)
        {{-1,  1, -1,  0, 1, 0}, {-1,  1,  1,  0, 1, 0}, { 1,  1,  1,  0, 1, 0}, { 1,  1, -1,  0, 1, 0}},
        // -Y (normal 0,-1,0)
        {{-1, -1,  1,  0,-1, 0}, {-1, -1, -1,  0,-1, 0}, { 1, -1, -1,  0,-1, 0}, { 1, -1,  1,  0,-1, 0}},
        // +Z (normal 0,0,1)
        {{-1, -1,  1,  0, 0, 1}, { 1, -1,  1,  0, 0, 1}, { 1,  1,  1,  0, 0, 1}, {-1,  1,  1,  0, 0, 1}},
        // -Z (normal 0,0,-1)
        {{ 1, -1, -1,  0, 0,-1}, {-1, -1, -1,  0, 0,-1}, {-1,  1, -1,  0, 0,-1}, { 1,  1, -1,  0, 0,-1}},
    };
    // clang-format on

    CubeMesh out;
    out.vertices.reserve(24 * 6);
    for (int f = 0; f < 6; ++f) {
        for (int v = 0; v < 4; ++v) {
            out.vertices.push_back(cx + faces[f][v][0] * halfExtent);
            out.vertices.push_back(cy + faces[f][v][1] * halfExtent);
            out.vertices.push_back(cz + faces[f][v][2] * halfExtent);
            out.vertices.push_back(faces[f][v][3]);
            out.vertices.push_back(faces[f][v][4]);
            out.vertices.push_back(faces[f][v][5]);
        }
    }

    out.indices.reserve(36);
    for (int f = 0; f < 6; ++f) {
        const uint16_t base = static_cast<uint16_t>(f * 4);
        out.indices.push_back(base);
        out.indices.push_back(static_cast<uint16_t>(base + 1));
        out.indices.push_back(static_cast<uint16_t>(base + 2));
        out.indices.push_back(base);
        out.indices.push_back(static_cast<uint16_t>(base + 2));
        out.indices.push_back(static_cast<uint16_t>(base + 3));
    }

    out.bounds = {cx - halfExtent, cy - halfExtent, cz - halfExtent,
                  cx + halfExtent, cy + halfExtent, cz + halfExtent};
    return out;
}

} // namespace blone
