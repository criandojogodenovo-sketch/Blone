#pragma once
// BLONEPRJ — Blone's own versioned project format (Phase 2).
//
// Layout (little-endian):
//   8B  magic "BLONEPRJ"
//   u32 formatVersion (currently 1; readers reject newer versions with a
//       clear error and never silently guess)
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
//       mesh (ALIVE elements only; pools are rebuilt on load):
//         u32 vertexCount; per vertex: u32 id, 3 x f32 position
//         u32 heCount;     per he:    u32 id, origin, twin, next, prev, face, edge
//         u32 edgeCount;   per edge:  u32 id, he
//         u32 faceCount;   per face:  u32 id, he
//         u32 nextVid, nextHid, nextEid, nextFid
//         u32 selVCount + ids; u32 selECount + ids; u32 selFCount + ids
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

constexpr uint32_t kFormatVersion = 1;
constexpr size_t kMaxProjectObjects = 32;
constexpr size_t kMaxNameBytes = 256;

// Serializes the scene; returns an empty buffer on failure (should not happen
// for scenes that pass validate()).
std::vector<uint8_t> save(const Scene& scene);

// Parses and validates; on ANY error `out` is left untouched and a negative
// LoadError is returned.
int load(const std::vector<uint8_t>& bytes, Scene& out);

} // namespace blone::project
