#pragma once
// Undo/redo history with VERIFIABLE memory accounting (Phase 2 condition 3).
//
// Strategy (documented in ARCHITECTURE.md):
//  - Transform tools record position deltas (cheap, exact).
//  - Topology operations record full mesh snapshots (before + after).
//  - Adding/removing objects records the whole object.
//  - Every byte is counted: usedBytes() is the exact heap held by entries.
//  - A hard budget (default 24 MB) is enforced with a two-phase reservation:
//      1. reserve(costUpperBound) — BEFORE mutating anything — evicts the
//         oldest undo entries (and the whole redo branch) until the bound
//         fits; if even the bound does not fit, the operation is REJECTED
//         (ErrBudget) and the mesh is untouched.
//      2. push*(...) after a successful operation stores the entry with its
//         EXACT byte cost; unused reservation margin is released.
//  - Any new operation discards the redo branch (standard behaviour).
//  - Entries are capped at kMaxEntries; memory is capped by the budget —
//    whichever limit hits first.

#include "../mesh/HalfEdgeMesh.h"
#include "../scene/Scene.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace blone {

class History {
public:
    static constexpr uint64_t kDefaultBudget = 24ull * 1024 * 1024; // 24 MB
    static constexpr size_t kMaxEntries = 64;

    void setBudget(uint64_t bytes) { budget_ = bytes > 0 ? bytes : 1; }
    uint64_t budget() const { return budget_; }
    uint64_t usedBytes() const { return used_; }
    uint64_t pendingBytes() const { return pending_; }
    size_t undoDepth() const { return undo_.size(); }
    size_t redoDepth() const { return redo_.size(); }
    uint64_t lastEntryBytes() const { return lastEntryBytes_; }

    // Phase 1 of the reservation. Returns false (operation must be rejected)
    // when even the upper bound cannot fit the budget.
    bool reserve(uint64_t costUpperBound);
    void rollbackReservation(); // release a reservation without pushing
    // Phase 2: push entries (a reservation must be pending). The entry's
    // exact byte cost replaces the reserved bound.
    bool pushVerticesDelta(uint32_t objIndex,
                           std::vector<std::pair<ElemId, Vec3>> before,
                           std::vector<std::pair<ElemId, Vec3>> after);
    // Paint stroke (Fase 3): exact color deltas for ONE gesture. Restores
    // ONLY colors (sculpt entries restore only positions — each entry type
    // restores exactly the data its operation modified).
    bool pushColorDelta(uint32_t objIndex,
                        std::vector<std::pair<ElemId, Vec3>> before,
                        std::vector<std::pair<ElemId, Vec3>> after);
    bool pushMeshSwap(uint32_t objIndex, HalfEdgeMesh::Raw before,
                      HalfEdgeMesh::Raw after);
    // Mesh swap that ALSO restores the object selection on undo/redo
    // (stability correction: "undo/redo restaura também um estado de
    // seleção válido"). The selections are stored as-is; History::undo/redo
    // prune afterwards, so only alive ids survive.
    bool pushMeshSwapWithSelection(uint32_t objIndex, HalfEdgeMesh::Raw before,
                                   HalfEdgeMesh::Raw after,
                                   const Selection& selBefore,
                                   const Selection& selAfter);
    bool pushObjectAdded(uint32_t index, const SceneObject& data);
    bool pushObjectRemoved(uint32_t index, const SceneObject& data);

    bool undo(Scene& scene);
    bool redo(Scene& scene);
    void clear();

private:
    struct Entry {
        enum class Kind { VertDelta, ColorDelta, MeshSwap, ObjAdd, ObjRemove }
                kind = Kind::VertDelta;
        uint32_t objIndex = 0;
        uint64_t bytes = 0;
        // VertDelta / ColorDelta (positions / vertex colors)
        std::vector<std::pair<ElemId, Vec3>> beforePos;
        std::vector<std::pair<ElemId, Vec3>> afterPos;
        std::vector<std::pair<ElemId, Vec3>> beforeCol;
        std::vector<std::pair<ElemId, Vec3>> afterCol;
        // MeshSwap
        HalfEdgeMesh::Raw beforeMesh;
        HalfEdgeMesh::Raw afterMesh;
        bool hasSelections = false; // true => restore selBefore/selAfter
        Selection selBefore;
        Selection selAfter;
        // ObjAdd / ObjRemove
        SceneObject object;
    };

    static uint64_t meshRawBytes(const HalfEdgeMesh::Raw& r);
    static uint64_t objectBytes(const SceneObject& o);
    void dropOldestUndo();
    bool applyUndoToScene(Scene& scene, Entry& e);
    bool applyRedoToScene(Scene& scene, Entry& e);

    std::vector<Entry> undo_; // front = oldest, back = newest
    std::vector<Entry> redo_; // back = next redo target
    uint64_t budget_ = kDefaultBudget;
    uint64_t used_ = 0;
    uint64_t pending_ = 0;
    uint64_t lastEntryBytes_ = 0;
};

} // namespace blone
