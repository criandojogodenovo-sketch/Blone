#include "GltfLoader.h"
#include "GeometryLimits.h" // formatCount (numbered rejection messages, P1)

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

// ---- failure reason plumbing (hotfix D1): PT reasons per cgltf stage.
const char* resultReason(cgltf_result r) {
    switch (r) {
        case cgltf_result_file_not_found: return "ficheiro não encontrado";
        case cgltf_result_io_error: return "erro de leitura do ficheiro";
        case cgltf_result_unknown_format: return "não é um ficheiro glTF/GLB";
        case cgltf_result_invalid_json: return "JSON inválido";
        case cgltf_result_invalid_gltf: return "estrutura glTF inválida";
        case cgltf_result_legacy_gltf: return "glTF 1.0 não suportado";
        case cgltf_result_data_too_short: return "ficheiro truncado";
        case cgltf_result_out_of_memory: return "sem memória para ler o ficheiro";
        default: return "ficheiro glTF inválido";
    }
}

// Bitwise position equality for the degenerate-triangle rule (Math3D's
// Vec3 intentionally has no operator==; NaN never compares equal, which is
// the behavior we want: corrupt NaN geometry flows on to MeshBuilder).
bool samePos(const Vec3& a, const Vec3& b) {
    return std::memcmp(&a, &b, sizeof(Vec3)) == 0;
}

// ---- D5 (hotfix 2): missing external buffer names --------------------------

// Minimal percent-decode of a URI path component: %XX hex pairs become the
// raw byte; everything else (including '+' and stray '%') is kept as-is —
// we only need the FILE NAME to ask the user for, never a canonical URL.
void percentDecodeInPlace(std::string& s) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hexVal(s[i + 1]);
            const int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    s = std::move(out);
}

// Base name of a buffer URI: percent-decoded, path prefixes stripped (both
// separators — the file may come from any platform). Empty stays empty; the
// caller then falls back to the raw uri so the list is never misleading.
std::string bufferBaseName(const char* uri) {
    std::string s = uri ? uri : "";
    percentDecodeInPlace(s);
    const size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    return s;
}

// The buffers whose data never arrived AND that are file references (not
// GLB-embedded — uri==NULL — and not data-URIs). Only these can be fixed by
// the user picking the right folder; anything else is a real parse error.
std::vector<std::string> missingExternalBufferNames(const cgltf_data& data) {
    std::vector<std::string> out;
    for (size_t i = 0; i < data.buffers_count; ++i) {
        const cgltf_buffer& b = data.buffers[i];
        if (b.data != nullptr || b.uri == nullptr) continue;
        if (std::strncmp(b.uri, "data:", 5) == 0) continue; // inline, not a file
        std::string name = bufferBaseName(b.uri);
        if (name.empty()) name = b.uri; // never report an empty entry
        out.push_back(std::move(name));
    }
    return out;
}

} // namespace

std::optional<RawMeshData> GltfLoader::load(const std::string& path,
                                            std::string* err,
                                            IoProgress* progress,
                                            std::vector<std::string>* missingFiles) {
    if (path.empty()) {
        if (err) *err = "caminho vazio";
        return std::nullopt;
    }
    if (progress && progress->cancelled()) {
        if (err) *err = "importação cancelada";
        return std::nullopt;
    }

    cgltf_options options{};
    cgltf_data* data = nullptr;
    cgltf_result r = cgltf_parse_file(&options, path.c_str(), &data);
    if (r != cgltf_result_success) {
        if (err) *err = resultReason(r);
        return std::nullopt;
    }
    CgltfGuard guard(data);

    // ---- P4 PRE-COUNT from the JSON (anti-crash): the accessor counts are
    // already known after the parse — a document above the hard ceiling is
    // rejected BEFORE cgltf_load_buffers reads/copies any buffer.
    if (progress) {
        size_t totalVerts = 0;
        size_t totalTris = 0;
        for (size_t mi = 0; mi < data->meshes_count; ++mi) {
            const cgltf_mesh& mesh = data->meshes[mi];
            for (size_t pi = 0; pi < mesh.primitives_count; ++pi) {
                const cgltf_primitive& prim = mesh.primitives[pi];
                if (prim.type != cgltf_primitive_type_triangles) continue;
                const cgltf_accessor* pos = findAttr(prim, "POSITION");
                if (pos) totalVerts += pos->count;
                if (prim.indices) totalTris += prim.indices->count / 3;
                else if (pos) totalTris += pos->count / 3;
            }
        }
        if (totalVerts > MeshLimits::maxVertices ||
            totalTris > MeshLimits::maxFaces) {
            if (err) {
                *err = "excede o teto do motor (" +
                       formatCount(MeshLimits::maxVertices) + " v / " +
                       formatCount(MeshLimits::maxFaces) + " f); documento tem " +
                       formatCount(totalVerts) + " v e " +
                       formatCount(totalTris) + " triângulos";
            }
            return std::nullopt;
        }
        progress->set(300); // JSON parsed + counted (buffers not yet loaded)
    }

    // uri_root = the document path: external .bin buffers of a .gltf file
    // resolve next to it. GLB buffers are embedded and need no root.
    r = cgltf_load_buffers(&options, data, path.c_str());
    if (r != cgltf_result_success) {
        // D5 (hotfix 2): a .gltf whose EXTERNAL .bin files are absent fails
        // here (SAF copied only the document; the .bin stayed in Downloads).
        // Those buffers are identifiable: data never arrived + file URI.
        // Report their decoded BASE names so the Kotlin layer can ask for
        // the folder that contains them (IMPORT_NEEDS_FILES + retry).
        std::vector<std::string> missing = missingExternalBufferNames(*data);
        if (!missing.empty()) {
            if (missingFiles) *missingFiles = std::move(missing);
            if (err) {
                *err = "o modelo referencia ficheiros externos que não "
                       "foram encontrados junto do documento";
            }
            return std::nullopt;
        }
        if (err) {
            *err = std::string("buffers não carregados: ") + resultReason(r);
        }
        return std::nullopt;
    }
    r = cgltf_validate(data);
    if (r != cgltf_result_success) {
        if (err) {
            *err = std::string("validação glTF falhou: ") + resultReason(r);
        }
        return std::nullopt;
    }
    if (progress) progress->set(400);
    return extractRaw(data, err, progress);
}

std::optional<RawMeshData> GltfLoader::extractRaw(const cgltf_data* data,
                                                  std::string* err,
                                                  IoProgress* progress) {
    if (!data) {
        if (err) *err = "documento glTF nulo";
        return std::nullopt;
    }
    RawMeshData out;
    size_t done = 0; // primitives processed (progress baseline)

    for (size_t mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& mesh = data->meshes[mi];
        for (size_t pi = 0; pi < mesh.primitives_count; ++pi) {
            const cgltf_primitive& prim = mesh.primitives[pi];
            // P4: periodic cancel/progress checks during extraction.
            if (progress) {
                if (++done % 8 == 0) {
                    if (progress->cancelled()) {
                        if (err) *err = "importação cancelada";
                        return std::nullopt;
                    }
                    const int p = 400 + static_cast<int>(done % 1000) / 4;
                    progress->set(p < 900 ? p : 900);
                }
            }
            if (prim.type != cgltf_primitive_type_triangles) continue;

            const cgltf_accessor* posAcc = findAttr(prim, "POSITION");
            if (!posAcc) {
                if (err) *err = "primitiva sem POSITION (obrigatório)";
                return std::nullopt; // mandatory
            }
            if (posAcc->type != cgltf_type_vec3) {
                if (err) *err = "POSITION não é vec3";
                return std::nullopt;
            }
            if (posAcc->count == 0) continue; // empty primitive: skip

            const cgltf_accessor* nrmAcc = findAttr(prim, "NORMAL");
            if (nrmAcc && (nrmAcc->type != cgltf_type_vec3 ||
                           nrmAcc->count != posAcc->count)) {
                if (err) *err = "NORMAL incompatível com POSITION";
                return std::nullopt;
            }
            const cgltf_accessor* uvAcc = findAttr(prim, "TEXCOORD_0");
            if (uvAcc && (uvAcc->type != cgltf_type_vec2 ||
                          uvAcc->count != posAcc->count)) {
                if (err) *err = "TEXCOORD_0 incompatível com POSITION";
                return std::nullopt;
            }
            const cgltf_accessor* colAcc = findAttr(prim, "COLOR_0");
            if (colAcc && colAcc->type != cgltf_type_vec3 &&
                colAcc->type != cgltf_type_vec4) {
                if (err) *err = "COLOR_0 tem um tipo não suportado";
                return std::nullopt; // never guess at exotic color layouts
            }
            if (colAcc && colAcc->count != posAcc->count) {
                if (err) *err = "COLOR_0 incompatível com POSITION";
                return std::nullopt;
            }
            if (posAcc->count > std::numeric_limits<int>::max()) {
                if (err) *err = "geometria excede o limite de vértices";
                return std::nullopt;
            }
            const size_t count = posAcc->count;
            // Hard ceiling guard (missão F4-LIMITS, P1): reject BEFORE the
            // big allocations below, with the numbers in the message (R8).
            // The runtime per-profile gate lives in the I/O worker.
            if (out.positions.size() + count > MeshLimits::maxVertices) {
                if (err) {
                    *err = "excede o teto de vértices do motor (" +
                           formatCount(MeshLimits::maxVertices) +
                           "; ficheiro tem " +
                           formatCount(out.positions.size() + count) + ")";
                }
                return std::nullopt;
            }

            // ---- vertices (concatenated with an offset) ----------------
            const size_t base = out.positions.size();
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
                    if (err) *err = "POSITION ilegível (buffer corrompido?)";
                    return std::nullopt;
                }
                out.positions[base + i] = Vec3{tmp[0], tmp[1], tmp[2]};
                if (nrmAcc) {
                    if (!cgltf_accessor_read_float(nrmAcc, i, tmp.data(), 4)) {
                        if (err) *err = "NORMAL ilegível (buffer corrompido?)";
                        return std::nullopt;
                    }
                    out.normals[base + i] = Vec3{tmp[0], tmp[1], tmp[2]};
                }
                if (uvAcc) {
                    if (!cgltf_accessor_read_float(uvAcc, i, tmp.data(), 4)) {
                        if (err) *err = "TEXCOORD_0 ilegível (buffer corrompido?)";
                        return std::nullopt;
                    }
                    out.uvs[base + i] = Vec2{tmp[0], tmp[1]};
                }
                if (colAcc) {
                    if (!cgltf_accessor_read_float(colAcc, i, tmp.data(), 4)) {
                        if (err) *err = "COLOR_0 ilegível (buffer corrompido?)";
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
                        {static_cast<int>(base + a), static_cast<int>(base + b),
                         static_cast<int>(base + c)});
                return true;
            };

            if (prim.indices) {
                const size_t icount = prim.indices->count;
                if (icount % 3 != 0) {
                    if (err) *err = "índices não múltiplos de 3 (dados corrompidos)";
                    return std::nullopt; // corrupt data
                }
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
                if (count % 3 != 0) {
                    if (err) *err = "primitiva não indexada não múltipla de 3";
                    return std::nullopt;
                }
                for (size_t t = 0; t + 2 < count; t += 3) {
                    emitTriangle(static_cast<int>(t), static_cast<int>(t + 1),
                                 static_cast<int>(t + 2));
                }
            }
        }
    }

    if (out.positions.empty() || out.faceIndices.empty()) {
        if (err) {
            *err = "glTF sem geometria de triângulos utilizável";
        }
        return std::nullopt; // no usable TRIANGLES geometry
    }
    return out;
}

} // namespace blone::io
