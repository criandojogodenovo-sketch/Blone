#pragma once
// core/io — Fase 4: glTF 2.0 / GLB loader via cgltf (the ONLY parser —
// hand-written JSON/binary parsing is explicitly forbidden here).
//
// Pipeline: cgltf_parse_file -> cgltf_load_buffers (uri_root = the file
// path, so external .bin buffers of .gltf files resolve next to the
// document) -> cgltf_validate -> extractRaw. A cgltf_data RAII guard calls
// cgltf_free on EVERY path.
//
// Extraction contract (geometry only — nodes/materials/skins ignored):
//  - primitives of type TRIANGLES only; other primitive types are skipped;
//  - POSITION attribute is MANDATORY (missing -> load fails);
//  - NORMAL, TEXCOORD_0 and COLOR_0 are optional; COLOR_0 must be VEC3 or
//    VEC4 (alpha dropped), NORMAL VEC3, TEXCOORD_0 VEC2 — anything else
//    fails the load instead of being guessed at;
//  - primitives concatenate with a vertex offset; indices u8/u16/u32 go
//    through cgltf_accessor_read_index and attributes through
//    cgltf_accessor_read_float — raw buffer pointers are never dereferenced;
//  - degenerate triangles (repeated corner indices / bitwise-equal
//    positions) are SKIPPED, not fatal;
//  - a triangle whose index list is not a multiple of 3 is malformed
//    (fails) — a half-emitted triangle is corrupt data, never trimmed;
//  - color parallelism: when a primitive carries COLOR_0 (or colors were
//    already emitted by an earlier primitive), the rule
//      if (colAcc || !out.colors.empty()) out.colors.resize(base+count, white)
//    keeps colors parallel to positions across primitives; primitives
//    without COLOR_0 then default to white. Empty global colors array
//    means "all white" for MeshBuilder.
//
// Failure contract: any parse/load/validate/accessor error returns
// std::nullopt with zero side effects (the cgltf handle is freed; the scene
// is never touched by this class).

#include "MeshBuilder.h"

#include <optional>
#include <string>

struct cgltf_data;

namespace blone::io {

class GltfLoader {
public:
    // Parses and validates the glTF/GLB document at `path` and extracts the
    // geometry of all TRIANGLES primitives. When `err` is non-null it
    // receives a human-readable PT reason for the failure (hotfix D1: toast
    // + logcat BloneIO); untouched on success. The default nullptr keeps
    // the previous signature and every existing caller working.
    static std::optional<RawMeshData> load(const std::string& path,
                                           std::string* err = nullptr);

    // Extracts RawMeshData from an ALREADY parsed/loaded/validated cgltf
    // document (exposed for tests; load() is the production entry point).
    // Same `err` contract as load().
    static std::optional<RawMeshData> extractRaw(const cgltf_data* data,
                                                 std::string* err = nullptr);
};

} // namespace blone::io
