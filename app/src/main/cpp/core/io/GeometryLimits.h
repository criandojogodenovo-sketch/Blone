#pragma once
// core/io — RUNTIME geometry limits per device profile (missão F4-LIMITS).
//
// The HARD ceiling (MeshLimits, HalfEdgeMesh.h) is the engine's absolute
// safety net. The USER-facing decision is made HERE, per device profile:
//
//   Perfil | editVerts | editTris | viewTris  | historyBytes
//   -------+-----------+----------+-----------+-------------
//   Baixo  |   40.000  |   80.000 |   500.000 |      8 MB
//   Médio  |   75.000  |  150.000 | 1.000.000 |     16 MB
//   Alto   |  150.000  |  300.000 | 2.000.000 |     24 MB
//
// Semantics (enforced by the I/O worker BEFORE any heavy allocation):
//   V <= editVerts AND tris <= editTris  -> EDITABLE mesh (HalfEdgeMesh);
//   else tris <= viewTris                -> READ-ONLY render mesh (P3);
//   else                                 -> REJECTED with a complete PT
//                                           reason carrying the numbers.
//
// The table satisfies the owner-approved acceptance criteria (P8): 50k tri
// imports stay editable on every profile; 200k tri is editable only on
// Alto (read elsewhere); 500k/1M tri import as read-only without a crash;
// 1M read-only peaks well under 300 MB (indexed render mesh, no half-edge).
//
// Memory per element (documented, ARCHITECTURE.md §10):
//   editable (HalfEdgeMesh)  ~250-350 B per triangle (pools + slot maps +
//                            pairMap + history upper bound);
//   read-only (RenderMesh)   ~30 B per triangle (indexed positions/colors +
//                            uint32 indices; normals come from the shader).
//
// Classification and the PT reason strings are PURE and host-testable — the
// messages must carry the numbers (R8: never a bare "Falha na importação").

#include "../mesh/HalfEdgeMesh.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace blone::io {

enum class LimitsProfile : int { Baixo = 0, Medio = 1, Alto = 2 };

struct RuntimeLimits {
    size_t editVerts = 0;
    size_t editTris = 0;
    size_t viewTris = 0;
    uint64_t historyBytes = 0;
};

// The approved table (single source of truth; P6 exposes it in the UI).
inline RuntimeLimits limitsFor(LimitsProfile p) {
    switch (p) {
        case LimitsProfile::Baixo:
            return {40000, 80000, 500000, 8ull * 1024 * 1024};
        case LimitsProfile::Alto:
            return {150000, 300000, 2000000, 24ull * 1024 * 1024};
        case LimitsProfile::Medio:
        default:
            return {75000, 150000, 1000000, 16ull * 1024 * 1024};
    }
}

inline const char* profileName(LimitsProfile p) {
    switch (p) {
        case LimitsProfile::Baixo: return "Baixo";
        case LimitsProfile::Alto: return "Alto";
        case LimitsProfile::Medio: return "Médio";
    }
    return "Médio";
}

// The CURRENT process profile. Default Médio until the UI sets a saved /
// RAM-appropriate one (P6). Atomic: workers read it while the UI writes it.
inline std::atomic<int>& currentProfileSlot() {
    static std::atomic<int> slot{static_cast<int>(LimitsProfile::Medio)};
    return slot;
}

inline LimitsProfile currentProfile() {
    const int v = currentProfileSlot().load(std::memory_order_relaxed);
    if (v >= 0 && v <= 2) return static_cast<LimitsProfile>(v);
    return LimitsProfile::Medio;
}

inline void setCurrentProfile(LimitsProfile p) {
    currentProfileSlot().store(static_cast<int>(p), std::memory_order_relaxed);
}

// ---- pure classification (host-tested) -------------------------------------

enum class ImportClass : int { Editable = 0, ReadOnly = 1, Rejected = 2 };

// Classifies a parsed file against the RUNTIME limits of `p`.
// `verts` = positions in the file, `tris` = exact triangle count
// (sum of max(1, sides-2) over faces — computed by the caller).
inline ImportClass classifyImport(size_t verts, size_t tris,
                                  const RuntimeLimits& lim) {
    if (verts > MeshLimits::maxVertices || tris > MeshLimits::maxFaces) {
        return ImportClass::Rejected; // hard ceiling (defensive; loaders
                                      // already guard their own allocations)
    }
    if (verts <= lim.editVerts && tris <= lim.editTris) {
        return ImportClass::Editable;
    }
    if (tris <= lim.viewTris) {
        return ImportClass::ReadOnly;
    }
    return ImportClass::Rejected;
}

// PT thousands separator (1.000.000) so every user-facing number reads
// naturally; digits only, no locale dependency.
inline std::string formatCount(uint64_t n) {
    std::string digits = std::to_string(n);
    size_t i = digits.size() > 3 ? digits.size() - 3 : 0;
    while (i > 0) {
        digits.insert(i, ".");
        i = i > 3 ? i - 3 : 0;
    }
    return digits;
}

// Complete PT reason for an EDIT-limit rejection (P1: the file parses fine
// but is above the profile's edit budget). Carries file counts, the active
// profile and its numbers — the Kotlin dialog composes the suggestions
// around this text. P3 routes the (<= viewTris) slice to read mode instead.
inline std::string reasonAboveEditLimit(LimitsProfile p, size_t verts,
                                        size_t tris) {
    const RuntimeLimits lim = limitsFor(p);
    return "ficheiro acima do limite de edição do perfil " +
           std::string(profileName(p)) + " (" + formatCount(lim.editTris) +
           " triângulos / " + formatCount(lim.editVerts) +
           " vértices); o ficheiro tem " + formatCount(tris) +
           " triângulos e " + formatCount(verts) + " vértices";
}

// Complete PT reason for a VIEW-limit rejection (P3+: above read-mode too).
inline std::string reasonAboveViewLimit(LimitsProfile p, size_t verts,
                                        size_t tris) {
    const RuntimeLimits lim = limitsFor(p);
    return "ficheiro acima do limite de visualização do perfil " +
           std::string(profileName(p)) + " (" + formatCount(lim.viewTris) +
           " triângulos); o ficheiro tem " + formatCount(tris) +
           " triângulos e " + formatCount(verts) + " vértices";
}

// Complete PT reason for a HARD-ceiling rejection (last-resort guard).
inline std::string reasonAboveHardCeiling(size_t verts, size_t tris) {
    return "ficheiro acima do teto absoluto do motor (" +
           formatCount(MeshLimits::maxVertices) + " vértices / " +
           formatCount(MeshLimits::maxFaces) + " faces); o ficheiro tem " +
           formatCount(tris) + " triângulos e " + formatCount(verts) +
           " vértices";
}

} // namespace blone::io
