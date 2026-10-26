#pragma once
// BLONEPRJ — Blone's own versioned project format.
//
// VERSION HISTORY
//  v1 (Fase 2): geometry + topology + selection, no vertex colors.
//  v2 (Fase 3): adds 3 x f32 albedo color per vertex (see §Payload).
//  v3 (F4-LIMITS): adds a per-object `editable` flag. Read-only objects
//      carry their geometry as an indexed RENDER MESH (positions+colors+
//      u32 triangle indices) instead of half-edge structures.
//  v4 (Fase 5.1): adds a SPARSE per-corner uv section to the half-edge
//      payload — one (heId, u, v) triple per alive half-edge that carries
//      stored uv data (seams included). Meshes without uvs write a zero
//      count and the section costs 4 bytes.
//
// Compatibility contract (docs/PROJECT_FORMAT.md):
//  - The reader accepts v1, v2, v3 AND v4 (a v1/v2/v3 file loads with the
//    default vertex color kDefaultVertexColor / no uvs and editable=true —
//    files from before each extension look exactly unchanged).
//  - The writer emits the CURRENT version (4).
//  - Newer-than-known versions are rejected with ErrVersion and a clear
//    user-facing message — never guessed at.
//
// Layout (little-endian):
//   8B  magic "BLONEPRJ"
//   u32 formatVersion (4 since Fase 5.1; readers reject newer versions
//       with a clear error and never silently guess)
//   u64 payloadLength
//   u32 CRC32 (IEEE, poly 0xEDB88320) of the payload
//   payload:
//     i32 activeObject (-1 when none)
//     u8  editMode     (0/1)
//     u8  selectionMode (0 vertex, 1 edge, 2 face)
//     u8  multiSelect  (0/1)
//     u8  reserved
//     u32 objectCount
//     per object:
//       u32 nameLength, UTF-8 name bytes (<= 256)
//       3 x f32 position
//       v3 ONLY: u8 editable (1 editable, 0 read-only)
//       editable == 1 (half-edge mesh — ALIVE elements only; pools are
//       rebuilt on load):
//         u32 vertexCount; per vertex: u32 id, 3 x f32 position
//             v2 ONLY: 3 x f32 color (albedo RGB in [0,1])
//         u32 heCount;     per he:    u32 id, origin, twin, next, prev, face, edge
//         v4 ONLY: u32 uvCount; per entry: u32 heId, f32 u, f32 v
//             (sparse — only alive half-edges with stored per-corner uvs;
//             the uv of a corner lives on its face-side half-edge, so a
//             seam vertex keeps one value per adjacent face)
//         u32 edgeCount;   per edge:  u32 id, he
//         u32 faceCount;   per face:  u32 id, he
//         u32 nextVid, nextHid, nextEid, nextFid
//         u32 selVCount + ids; u32 selECount + ids; u32 selFCount + ids
//       editable == 0 (READ-ONLY render mesh — F4-LIMITS v3):
//         u32 vertexCount; per vertex: 3 x f32 position, 3 x f32 color
//         u32 indexCount (multiple of 3); per index: u32
//         (selection is always empty for read-only objects)
//
// Loading validates magic, version, sizes, counts, CRC and full topology
// in a TEMPORARY scene; the caller's scene is only replaced on success, so
// an invalid file can never destroy the currently open project.

#include "../scene/Scene.h"

#include <cstdint>
#include <vector>

namespace blone::project {

enum LoadError : int {
    Ok = 0,
    ErrMagic = -1,
    ErrVersion = -2,
    ErrSize = -3,
    ErrCrc = -4,
    ErrLimits = -5,
    ErrTopology = -6,
    ErrTruncated = -7,
    ErrIndex = -8,
};

constexpr uint32_t kFormatVersion = 4;
// Oldest version the reader still accepts (v1 = pre-color files).
constexpr uint32_t kOldestSupportedVersion = 1;
constexpr size_t kMaxProjectObjects = 32;
constexpr size_t kMaxNameBytes = 256;

// Serializes the scene; returns an empty buffer on failure (should not happen
// for scenes that pass validate()).
std::vector<uint8_t> save(const Scene& scene);

// Parses and validates; on ANY error `out` is left untouched and a negative
// LoadError is returned.
int load(const std::vector<uint8_t>& bytes, Scene& out);

} // namespace blone::project
