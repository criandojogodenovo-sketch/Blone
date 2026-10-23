#include "RenderMesh.h"

#include <algorithm>

namespace blone::render {

uint64_t RenderMesh::memoryBytes() const {
    return static_cast<uint64_t>(positions.capacity()) * sizeof(Vec3) +
           static_cast<uint64_t>(colors.capacity()) * sizeof(Vec3) +
           static_cast<uint64_t>(indices.capacity()) * sizeof(uint32_t) +
           sizeof(RenderMesh);
}

RenderMesh buildRenderMesh(const blone::io::RawMeshData& raw) {
    RenderMesh m;
    m.positions = raw.positions;
    m.colors = raw.colors;

    // Fan triangulation of every face (n-2 triangles) — see the header
    // contract. Degenerate corners (repeated index) are skipped, mirroring
    // the glTF degenerate rule.
    m.indices.reserve(raw.faceIndices.size() * 3);
    for (const std::vector<int>& loop : raw.faceIndices) {
        if (loop.size() < 3) continue; // defensive; MeshBuilder rejects anyway
        for (size_t k = 1; k + 1 < loop.size(); ++k) {
            const int a = loop[0];
            const int b = loop[k];
            const int c = loop[k + 1];
            if (a == b || b == c || a == c) continue;
            m.indices.push_back(static_cast<uint32_t>(a));
            m.indices.push_back(static_cast<uint32_t>(b));
            m.indices.push_back(static_cast<uint32_t>(c));
        }
    }
    m.triangleCount = m.indices.size() / 3;

    // Bounds + centroid, computed once (the renderer/pivot must not walk
    // the positions every frame).
    Vec3 acc{};
    size_t n = 0;
    if (!m.positions.empty()) {
        Aabb b;
        b.minX = b.maxX = m.positions[0].x;
        b.minY = b.maxY = m.positions[0].y;
        b.minZ = b.maxZ = m.positions[0].z;
        for (const Vec3& p : m.positions) {
            b.minX = std::min(b.minX, p.x);
            b.minY = std::min(b.minY, p.y);
            b.minZ = std::min(b.minZ, p.z);
            b.maxX = std::max(b.maxX, p.x);
            b.maxY = std::max(b.maxY, p.y);
            b.maxZ = std::max(b.maxZ, p.z);
            acc = acc + p;
            ++n;
        }
        m.bounds = b;
    }
    if (n > 0) m.centroid = acc * (1.0f / static_cast<float>(n));
    return m;
}

blone::io::RawMeshData renderMeshToRaw(const RenderMesh& m) {
    blone::io::RawMeshData raw;
    raw.positions = m.positions;
    raw.colors = m.colors;
    raw.faceIndices.reserve(m.triangleCount);
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        raw.faceIndices.push_back({static_cast<int>(m.indices[t]),
                                   static_cast<int>(m.indices[t + 1]),
                                   static_cast<int>(m.indices[t + 2])});
    }
    return raw;
}

} // namespace blone::render
