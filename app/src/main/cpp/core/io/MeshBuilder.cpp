#include "MeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace blone::io {

namespace {

bool finite3(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

float clamp01(float v) {
    return std::min(1.0f, std::max(0.0f, v));
}

} // namespace

std::optional<HalfEdgeMesh> MeshBuilder::build(const RawMeshData& data,
                                               std::string* err) {
    // 1) Empty input is always a rejection (nothing to build).
    if (data.positions.empty() || data.faceIndices.empty()) {
        if (err) *err = "sem vértices ou sem faces";
        return std::nullopt;
    }

    // 2) Global geometry limits — rejected BEFORE any work happens.
    if (data.positions.size() > MeshLimits::maxVertices) {
        if (err) *err = "excede o limite de vértices";
        return std::nullopt;
    }
    if (data.faceIndices.size() > MeshLimits::maxFaces) {
        if (err) *err = "excede o limite de faces";
        return std::nullopt;
    }

    // 3) Parallel arrays: empty OR exactly parallel. Nothing else.
    if (!data.normals.empty() && data.normals.size() != data.positions.size()) {
        if (err) *err = "normais não paralelas às posições";
        return std::nullopt;
    }
    if (!data.colors.empty() && data.colors.size() != data.positions.size()) {
        if (err) *err = "cores não paralelas às posições";
        return std::nullopt;
    }
    if (!data.uvs.empty() && data.uvs.size() != data.positions.size()) {
        if (err) *err = "uv não paralelas às posições";
        return std::nullopt;
    }

    // 4) Positions must be finite. Colors must be finite too (out-of-range
    //    finite components are clamped — same policy as the project loader;
    //    non-finite colors are corrupt data, never guessed at).
    for (const Vec3& p : data.positions) {
        if (!finite3(p.x, p.y, p.z)) {
            if (err) *err = "posição não finita (NaN/Inf)";
            return std::nullopt;
        }
    }
    for (const Vec3& c : data.colors) {
        if (!finite3(c.x, c.y, c.z)) {
            if (err) *err = "cor não finita (NaN/Inf)";
            return std::nullopt;
        }
    }

    // 5) Face sanity: 3..maxFaceSides corners, indices in range, no vertex
    //    repeated inside one loop. Checked up front so the construction
    //    below cannot fail halfway through a face.
    const int maxIndex = static_cast<int>(data.positions.size()) - 1;
    for (const std::vector<int>& loop : data.faceIndices) {
        if (loop.size() < 3 || loop.size() > MeshLimits::maxFaceSides) {
            if (err) {
                *err = loop.size() < 3
                           ? "face com menos de 3 lados"
                           : "face com mais de " +
                                 std::to_string(MeshLimits::maxFaceSides) + " lados";
            }
            return std::nullopt;
        }
        std::unordered_set<int> seen;
        seen.reserve(loop.size() * 2);
        for (const int idx : loop) {
            if (idx < 0 || idx > maxIndex) {
                if (err) *err = "índice de vértice fora do intervalo";
                return std::nullopt;
            }
            if (!seen.insert(idx).second) {
                if (err) *err = "vértice repetido no contorno de uma face";
                return std::nullopt; // duplicate
            }
        }
    }

    // 6) Construct through the EXISTING pipeline. Everything so far ran on
    //    read-only data; from here the mesh is a local temporary — any
    //    failure below simply discards it (no scene is touched anywhere).
    HalfEdgeMesh mesh;
    std::vector<ElemId> ids;
    ids.reserve(data.positions.size());
    const bool hasColors = !data.colors.empty();
    for (size_t i = 0; i < data.positions.size(); ++i) {
        const ElemId id = mesh.addVertex(data.positions[i]);
        if (id == kNull) {
            if (err) *err = "limite de vértices atingido durante a construção";
            return std::nullopt; // pool limit (defensive)
        }
        // Paint color: explicit when provided, white otherwise ("colors
        // vazio = tudo branco"). Clamp mirrors the project loader policy.
        mesh.vrt(id).color = hasColors
                                 ? Vec3{clamp01(data.colors[i].x),
                                        clamp01(data.colors[i].y),
                                        clamp01(data.colors[i].z)}
                                 : Vec3{1.0f, 1.0f, 1.0f};
        ids.push_back(id);
    }
    for (const std::vector<int>& loop : data.faceIndices) {
        std::vector<ElemId> face;
        face.reserve(loop.size());
        for (const int idx : loop) face.push_back(ids[static_cast<size_t>(idx)]);
        if (mesh.addFace(face) == kNull) {
            if (err) *err = "face não construída (topologia não suportada)";
            return std::nullopt;
        }
    }

    // 7) Same finishing contract as every other construction site.
    mesh.relinkBoundaryLoops();
    if (!mesh.validate()) {
        if (err) *err = "validação topológica da malha falhou";
        return std::nullopt;
    }

    return mesh;
}

} // namespace blone::io
