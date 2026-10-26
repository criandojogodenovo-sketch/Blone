// gen_mesh.hpp — deterministic mesh GENERATOR for the big-import tests
// (missão F4-LIMITS, P8). Shared by the gen_mesh CLI tool and the test
// suite: N triangles as a deterministic triangle soup (positions encode
// their index so corruption is detectable). No binaries are committed —
// files are generated at test runtime into tmp.
#pragma once

#include "../../core/io/MeshBuilder.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace blone::gen {

// Generates exactly `triangles` triangles as a CONNECTED triangulated grid
// (quads split into 2 triangles — vertex density ≈ tris/2 + O(sqrt(tris)),
// like real modeling files). The grid is a FULL rectangle (cols x rows
// factorisation of triangles/2) so every generated vertex belongs to a face
// (the topology validator rejects isolated vertices — I6). Odd quotas end
// with one standalone triangle island far from the grid. Coordinates are
// exact small integers: OBJ text, float32 GLB and half-edge builds all
// round-trip bit-exactly.
inline void generateTriangles(size_t triangles, blone::io::RawMeshData& out) {
    out.positions.clear();
    out.colors.clear();
    out.faceIndices.clear();
    if (triangles == 0) return;
    const size_t quads = triangles / 2;
    const bool odd = (triangles % 2) != 0;
    size_t cols = 0, rows = 0;
    if (quads > 0) {
        cols = static_cast<size_t>(std::sqrt(static_cast<double>(quads)));
        if (cols == 0) cols = 1;
        while (quads % cols != 0) ++cols; // exact factorisation (fast: O(sqrt))
        rows = quads / cols;
    }

    // Grid vertices (every one of them is used by the quads below).
    out.positions.reserve((cols + 1) * (rows + 1) + 3);
    for (size_t r = 0; r <= rows; ++r) {
        for (size_t c = 0; c <= cols; ++c) {
            out.positions.push_back(blone::Vec3{static_cast<float>(c),
                                                static_cast<float>(r), 0.0f});
        }
    }
    auto vid = [cols](size_t r, size_t c) {
        return static_cast<int>(r * (cols + 1) + c);
    };

    out.faceIndices.reserve(triangles);
    for (size_t r = 0; r < rows; ++r) {
        for (size_t c = 0; c < cols; ++c) {
            const int a = vid(r, c);
            const int b = vid(r, c + 1);
            const int d = vid(r + 1, c + 1);
            const int e = vid(r + 1, c);
            out.faceIndices.push_back({a, b, d});
            out.faceIndices.push_back({a, d, e});
        }
    }

    // Odd quota: one standalone triangle island (valid topology — a second
    // boundary loop), placed far from the grid so it dedups to fresh verts.
    if (odd) {
        const float fx = static_cast<float>(cols) + 10.0f;
        const size_t base = out.positions.size();
        out.positions.push_back(blone::Vec3{fx, 0.0f, 0.0f});
        out.positions.push_back(blone::Vec3{fx + 1.0f, 0.0f, 0.0f});
        out.positions.push_back(blone::Vec3{fx + 0.5f, 1.0f, 0.0f});
        out.faceIndices.push_back({static_cast<int>(base),
                                   static_cast<int>(base + 1),
                                   static_cast<int>(base + 2)});
    }
}

// Writes a text OBJ (1-based faces).
inline bool writeObj(const blone::io::RawMeshData& raw, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    bool ok = true;
    for (const blone::Vec3& p : raw.positions) {
        if (std::fprintf(f, "v %.1f %.1f %.1f\n", static_cast<double>(p.x),
                         static_cast<double>(p.y), static_cast<double>(p.z)) < 0) {
            ok = false;
            break;
        }
    }
    if (ok) {
        for (const auto& loop : raw.faceIndices) {
            if (loop.size() < 3) continue;
            if (std::fprintf(f, "f %d %d %d\n", loop[0] + 1, loop[1] + 1,
                             loop[2] + 1) < 0) {
                ok = false;
                break;
            }
        }
    }
    std::fclose(f);
    return ok;
}

// Minimal glTF 2.0 GLB writer (positions float32 VEC3 + indices uint32).
inline bool writeGlb(const blone::io::RawMeshData& raw, const std::string& path) {
    if (raw.positions.empty() || raw.faceIndices.empty()) return false;
    const uint32_t vertCount = static_cast<uint32_t>(raw.positions.size());
    const uint32_t idxCount = static_cast<uint32_t>(raw.faceIndices.size() * 3);
    const uint32_t posBytes = vertCount * 12;
    const uint32_t idxBytes = idxCount * 4;

    std::vector<uint8_t> bin(posBytes + idxBytes);
    for (uint32_t i = 0; i < vertCount; ++i) {
        const blone::Vec3& p = raw.positions[i];
        std::memcpy(bin.data() + i * 12 + 0, &p.x, 4);
        std::memcpy(bin.data() + i * 12 + 4, &p.y, 4);
        std::memcpy(bin.data() + i * 12 + 8, &p.z, 4);
    }
    uint32_t* idx = reinterpret_cast<uint32_t*>(bin.data() + posBytes);
    uint32_t k = 0;
    for (const auto& loop : raw.faceIndices) {
        idx[k++] = static_cast<uint32_t>(loop[0]);
        idx[k++] = static_cast<uint32_t>(loop[1]);
        idx[k++] = static_cast<uint32_t>(loop[2]);
    }
    // Accessor min/max (spec-recommended, keeps cgltf_validate happy).
    blone::Vec3 mn = raw.positions[0];
    blone::Vec3 mx = raw.positions[0];
    for (const blone::Vec3& p : raw.positions) {
        mn.x = std::fmin(mn.x, p.x); mn.y = std::fmin(mn.y, p.y); mn.z = std::fmin(mn.z, p.z);
        mx.x = std::fmax(mx.x, p.x); mx.y = std::fmax(mx.y, p.y); mx.z = std::fmax(mx.z, p.z);
    }
    char json[1024];
    std::snprintf(json, sizeof(json),
                  "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
                  "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
                  "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},"
                  "\"indices\":1,\"mode\":4}]}],"
                  "\"buffers\":[{\"byteLength\":%u}],"
                  "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,"
                  "\"byteLength\":%u,\"target\":34962},"
                  "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,"
                  "\"target\":34963}],"
                  "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
                  "\"count\":%u,\"type\":\"VEC3\",\"min\":[%.1f,%.1f,%.1f],"
                  "\"max\":[%.1f,%.1f,%.1f]},{\"bufferView\":1,"
                  "\"componentType\":5125,\"count\":%u,\"type\":\"SCALAR\"}]}",
                  posBytes + idxBytes, posBytes, posBytes, idxBytes, vertCount,
                  static_cast<double>(mn.x), static_cast<double>(mn.y),
                  static_cast<double>(mn.z), static_cast<double>(mx.x),
                  static_cast<double>(mx.y), static_cast<double>(mx.z),
                  idxCount);
    const uint32_t jsonLen = static_cast<uint32_t>(std::string(json).size());
    const uint32_t jsonPad = (4 - (jsonLen % 4)) % 4;
    const uint32_t binPad = (4 - (bin.size() % 4)) % 4;
    const uint32_t total =
            12 + 8 + jsonLen + jsonPad + 8 +
            static_cast<uint32_t>(bin.size()) + binPad;

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = true;
    auto w32 = [&](uint32_t v) {
        ok = ok && std::fwrite(&v, 4, 1, f) == 1;
    };
    const char gltfMagic[4] = {'g', 'l', 'T', 'F'};
    const char jsonType[4] = {'J', 'S', 'O', 'N'};
    const char binType[4] = {'B', 'I', 'N', 0};
    const char space = ' ';
    const char zero = 0;
    ok = ok && std::fwrite(gltfMagic, 1, 4, f) == 4;
    w32(2);            // version
    w32(total);        // total length
    w32(jsonLen + jsonPad);
    ok = ok && std::fwrite(jsonType, 1, 4, f) == 4;
    ok = ok && std::fwrite(json, 1, jsonLen, f) == jsonLen;
    for (uint32_t i = 0; i < jsonPad; ++i) ok = ok && std::fwrite(&space, 1, 1, f) == 1;
    w32(static_cast<uint32_t>(bin.size()) + binPad);
    ok = ok && std::fwrite(binType, 1, 4, f) == 4;
    ok = ok && std::fwrite(bin.data(), 1, bin.size(), f) == bin.size();
    for (uint32_t i = 0; i < binPad; ++i) ok = ok && std::fwrite(&zero, 1, 1, f) == 1;
    const bool closed = std::fclose(f) == 0;
    return ok && closed;
}

} // namespace blone::gen
