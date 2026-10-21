#include "GltfLoader.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "third_party/cgltf/cgltf.h"

namespace blone::io {

namespace {

// RAII guard: cgltf_free runs on EVERY path (documented contract).
struct CgltfGuard {
    cgltf_data* data = nullptr;
    explicit CgltfGuard(cgltf_data* d) : data(d) {}
    ~CgltfGuard() {
        if (data) cgltf_free(data);
    }
    CgltfGuard(const CgltfGuard&) = delete;
    CgltfGuard& operator=(const CgltfGuard&) = delete;
};

const cgltf_accessor* findAttr(const cgltf_primitive& prim, const char* name) {
    for (size_t i = 0; i < prim.attributes_count; ++i) {
        const cgltf_attribute& attr = prim.attributes[i];
        if (attr.name && std::strcmp(attr.name, name) == 0) return attr.data;
    }
    return nullptr;
}

// Bitwise position equality for the degenerate-triangle rule (Math3D's
// Vec3 intentionally has no operator==; NaN never compares equal, which is
// the behavior we want: corrupt NaN geometry flows on to MeshBuilder).
bool samePos(const Vec3& a, const Vec3& b) {
    return std::memcmp(&a, &b, sizeof(Vec3)) == 0;
}

} // namespace

std::optional<RawMeshData> GltfLoader::load(const std::string& path) {
    if (path.empty()) return std::nullopt;

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) !=
        cgltf_result_success) {
        return std::nullopt;
    }
    CgltfGuard guard(data);

    // uri_root = the document path: external .bin buffers of a .gltf file
    // resolve next to it. GLB buffers are embedded and need no root.
    if (cgltf_load_buffers(&options, data, path.c_str()) !=
        cgltf_result_success) {
        return std::nullopt;
    }
    if (cgltf_validate(data) != cgltf_result_success) {
        return std::nullopt;
    }
    return extractRaw(data);
}

std::optional<RawMeshData> GltfLoader::extractRaw(const cgltf_data* data) {
    if (!data) return std::nullopt;
    RawMeshData out;

    for (size_t mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& mesh = data->meshes[mi];
        for (size_t pi = 0; pi < mesh.primitives_count; ++pi) {
            const cgltf_primitive& prim = mesh.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            const cgltf_accessor* posAcc = findAttr(prim, "POSITION");
            if (!posAcc) return std::nullopt; // mandatory
            if (posAcc->type != cgltf_type_vec3) return std::nullopt;
            if (posAcc->count == 0) continue; // empty primitive: skip

            const cgltf_accessor* nrmAcc = findAttr(prim, "NORMAL");
            if (nrmAcc && (nrmAcc->type != cgltf_type_vec3 ||
                           nrmAcc->count != posAcc->count)) {
                return std::nullopt;
            }
            const cgltf_accessor* uvAcc = findAttr(prim, "TEXCOORD_0");
            if (uvAcc && (uvAcc->type != cgltf_type_vec2 ||
                          uvAcc->count != posAcc->count)) {
                return std::nullopt;
            }
            const cgltf_accessor* colAcc = findAttr(prim, "COLOR_0");
            if (colAcc && colAcc->type != cgltf_type_vec3 &&
                colAcc->type != cgltf_type_vec4) {
                return std::nullopt; // never guess at exotic color layouts
            }
            if (colAcc && colAcc->count != posAcc->count) return std::nullopt;
            if (posAcc->count > std::numeric_limits<int>::max()) {
                return std::nullopt;
            }

            // ---- vertices (concatenated with an offset) ----------------
            const size_t base = out.positions.size();
            const size_t count = posAcc->count;
            out.positions.resize(base + count);
            if (nrmAcc || !out.normals.empty()) {
                out.normals.resize(base + count, Vec3{0.0f, 0.0f, 0.0f});
            }
            if (uvAcc || !out.uvs.empty()) {
                out.uvs.resize(base + count, Vec2{0.0f, 0.0f});
            }
            if (colAcc || !out.colors.empty()) {
                out.colors.resize(base + count, Vec3{1.0f, 1.0f, 1.0f});
            }

            std::vector<float> tmp(4, 0.0f);
            for (size_t i = 0; i < count; ++i) {
                if (!cgltf_accessor_read_float(posAcc, i, tmp.data(), 4)) {
                    return std::nullopt;
                }
                out.positions[base + i] = Vec3{tmp[0], tmp[1], tmp[2]};
                if (nrmAcc) {
                    if (!cgltf_accessor_read_float(nrmAcc, i, tmp.data(), 4)) {
                        return std::nullopt;
                    }
                    out.normals[base + i] = Vec3{tmp[0], tmp[1], tmp[2]};
                }
                if (uvAcc) {
                    if (!cgltf_accessor_read_float(uvAcc, i, tmp.data(), 4)) {
                        return std::nullopt;
                    }
                    out.uvs[base + i] = Vec2{tmp[0], tmp[1]};
                }
                if (colAcc) {
                    if (!cgltf_accessor_read_float(colAcc, i, tmp.data(), 4)) {
                        return std::nullopt;
                    }
                    out.colors[base + i] = Vec3{tmp[0], tmp[1], tmp[2]};
                }
            }

            // ---- triangles ---------------------------------------------
            // Degenerate triangles (repeated corner indices or bitwise-
            // equal positions) are skipped, NOT fatal.
            auto emitTriangle = [&out, &base](int a, int b, int c) -> bool {
                const Vec3& pa = out.positions[base + static_cast<size_t>(a)];
                const Vec3& pb = out.positions[base + static_cast<size_t>(b)];
                const Vec3& pc = out.positions[base + static_cast<size_t>(c)];
                if (a == b || b == c || a == c) return true; // degenerate
                if (samePos(pa, pb) || samePos(pb, pc) || samePos(pa, pc)) {
                    return true;
                }
                out.faceIndices.push_back(
                        {base + a, base + b, base + c});
                return true;
            };

            if (prim.indices) {
                const size_t icount = prim.indices->count;
                if (icount % 3 != 0) return std::nullopt; // corrupt data
                for (size_t t = 0; t + 2 < icount; t += 3) {
                    const int a = static_cast<int>(
                            cgltf_accessor_read_index(prim.indices, t));
                    const int b = static_cast<int>(
                            cgltf_accessor_read_index(prim.indices, t + 1));
                    const int c = static_cast<int>(
                            cgltf_accessor_read_index(prim.indices, t + 2));
                    emitTriangle(a, b, c);
                }
            } else {
                // Non-indexed primitive: vertices are triangles in order.
                if (count % 3 != 0) return std::nullopt;
                for (size_t t = 0; t + 2 < count; t += 3) {
                    emitTriangle(static_cast<int>(t), static_cast<int>(t + 1),
                                 static_cast<int>(t + 2));
                }
            }
        }
    }

    if (out.positions.empty() || out.faceIndices.empty()) {
        return std::nullopt; // no usable TRIANGLES geometry
    }
    return out;
}

} // namespace blone::io
