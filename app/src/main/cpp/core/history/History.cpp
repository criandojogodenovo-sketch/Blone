#include "History.h"

#include <algorithm>
#include <type_traits>

namespace blone {

uint64_t History::meshRawBytes(const HalfEdgeMesh::Raw& r) {
    auto b = [](const auto& v) {
        return static_cast<uint64_t>(v.capacity()) *
               sizeof(typename std::decay_t<decltype(v)>::value_type);
    };
    return b(r.verts) + b(r.hes) + b(r.edges) + b(r.faces) + b(r.vSlot) +
           b(r.hSlot) + b(r.eSlot) + b(r.fSlot) + b(r.vFree) + b(r.hFree) +
           b(r.eFree) + b(r.fFree);
}

uint64_t History::objectBytes(const SceneObject& o) {
    return meshRawBytes(o.mesh.captureRaw()) + o.name.capacity() + sizeof(SceneObject);
}

void History::dropOldestUndo() {
    if (undo_.empty()) return;
    used_ -= undo_.front().bytes;
    undo_.erase(undo_.begin());
}

bool History::reserve(uint64_t costUpperBound) {
    // Any new operation invalidates the redo branch. The redo entries' bytes
    // are ALREADY excluded from used_ (undo() subtracted them), so clearing
    // the stack here must NOT subtract again — a uint64 underflow made every
    // post-undo reservation fail with ErrBudget (regression found by the
    // UI-modes test suite: "undo, depois qualquer operação -> limite
    // excedido").
    redo_.clear();
    pending_ = 0;
    if (costUpperBound > budget_) return false; // even the bound doesn't fit
    while (used_ + costUpperBound > budget_ && !undo_.empty()) {
        dropOldestUndo();
    }
    if (used_ + costUpperBound > budget_) return false;
    pending_ = costUpperBound;
    return true;
}

void History::rollbackReservation() { pending_ = 0; }

bool History::pushVerticesDelta(uint32_t objIndex,
                                std::vector<std::pair<ElemId, Vec3>> before,
                                std::vector<std::pair<ElemId, Vec3>> after) {
    if (pending_ == 0) return false;
    Entry e;
    e.kind = Entry::Kind::VertDelta;
    e.objIndex = objIndex;
    e.beforePos = std::move(before);
    e.afterPos = std::move(after);
    e.bytes = static_cast<uint64_t>(e.beforePos.size() + e.afterPos.size()) * 16ull +
              sizeof(Entry);
    undo_.push_back(std::move(e));
    used_ += undo_.back().bytes; // exact cost replaces the reserved bound
    pending_ = 0;
    lastEntryBytes_ = undo_.back().bytes;
    while (undo_.size() > kMaxEntries) dropOldestUndo();
    return true;
}

bool History::pushMeshSwap(uint32_t objIndex, HalfEdgeMesh::Raw before,
                           HalfEdgeMesh::Raw after) {
    if (pending_ == 0) return false;
    Entry e;
    e.kind = Entry::Kind::MeshSwap;
    e.objIndex = objIndex;
    e.beforeMesh = std::move(before);
    e.afterMesh = std::move(after);
    e.bytes = meshRawBytes(e.beforeMesh) + meshRawBytes(e.afterMesh) + sizeof(Entry);
    undo_.push_back(std::move(e));
    used_ += undo_.back().bytes; // exact cost replaces the reserved bound
    pending_ = 0;
    lastEntryBytes_ = undo_.back().bytes;
    while (undo_.size() > kMaxEntries) dropOldestUndo();
    return true;
}

bool History::pushMeshSwapWithSelection(uint32_t objIndex, HalfEdgeMesh::Raw before,
                                        HalfEdgeMesh::Raw after,
                                        const Selection& selBefore,
                                        const Selection& selAfter) {
    if (pending_ == 0) return false;
    Entry e;
    e.kind = Entry::Kind::MeshSwap;
    e.objIndex = objIndex;
    e.beforeMesh = std::move(before);
    e.afterMesh = std::move(after);
    e.hasSelections = true;
    e.selBefore = selBefore;
    e.selAfter = selAfter;
    const uint64_t selBytes =
            static_cast<uint64_t>(e.selBefore.verts.size() + e.selBefore.edges.size() +
                                  e.selBefore.faces.size() + e.selAfter.verts.size() +
                                  e.selAfter.edges.size() + e.selAfter.faces.size()) *
                    4ull +
            64ull;
    e.bytes = meshRawBytes(e.beforeMesh) + meshRawBytes(e.afterMesh) + selBytes +
              sizeof(Entry);
    undo_.push_back(std::move(e));
    used_ += undo_.back().bytes; // exact cost replaces the reserved bound
    pending_ = 0;
    lastEntryBytes_ = undo_.back().bytes;
    while (undo_.size() > kMaxEntries) dropOldestUndo();
    return true;
}

bool History::pushObjectAdded(uint32_t index, const SceneObject& data) {
    if (pending_ == 0) return false;
    Entry e;
    e.kind = Entry::Kind::ObjAdd;
    e.objIndex = index;
    e.object = data;
    e.bytes = objectBytes(e.object) + sizeof(Entry);
    undo_.push_back(std::move(e));
    used_ += undo_.back().bytes; // exact cost replaces the reserved bound
    pending_ = 0;
    lastEntryBytes_ = undo_.back().bytes;
    while (undo_.size() > kMaxEntries) dropOldestUndo();
    return true;
}

bool History::pushObjectRemoved(uint32_t index, const SceneObject& data) {
    if (pending_ == 0) return false;
    Entry e;
    e.kind = Entry::Kind::ObjRemove;
    e.objIndex = index;
    e.object = data;
    e.bytes = objectBytes(e.object) + sizeof(Entry);
    undo_.push_back(std::move(e));
    used_ += undo_.back().bytes; // exact cost replaces the reserved bound
    pending_ = 0;
    lastEntryBytes_ = undo_.back().bytes;
    while (undo_.size() > kMaxEntries) dropOldestUndo();
    return true;
}

bool History::applyUndoToScene(Scene& scene, Entry& e) {
    if (e.objIndex >= scene.objects.size()) return false;
    SceneObject& obj = scene.objects[e.objIndex];
    switch (e.kind) {
        case Entry::Kind::VertDelta:
            for (const auto& [id, pos] : e.beforePos) {
                if (obj.mesh.vertexAlive(id)) obj.mesh.vrt(id).pos = pos;
            }
            break;
        case Entry::Kind::MeshSwap:
            obj.mesh.restoreRaw(e.beforeMesh);
            if (e.hasSelections) obj.sel = e.selBefore; // pruned by undo()
            break;
        case Entry::Kind::ObjAdd:
            e.object = std::move(obj); // keep the object for redo
            scene.objects.erase(scene.objects.begin() + e.objIndex);
            break;
        case Entry::Kind::ObjRemove:
            scene.objects.insert(scene.objects.begin() + e.objIndex, std::move(e.object));
            break;
    }
    return true;
}

bool History::applyRedoToScene(Scene& scene, Entry& e) {
    if (e.objIndex > scene.objects.size()) return false;
    switch (e.kind) {
        case Entry::Kind::VertDelta: {
            if (e.objIndex >= scene.objects.size()) return false;
            SceneObject& obj = scene.objects[e.objIndex];
            for (const auto& [id, pos] : e.afterPos) {
                if (obj.mesh.vertexAlive(id)) obj.mesh.vrt(id).pos = pos;
            }
            break;
        }
        case Entry::Kind::MeshSwap: {
            if (e.objIndex >= scene.objects.size()) return false;
            scene.objects[e.objIndex].mesh.restoreRaw(e.afterMesh);
            if (e.hasSelections) {
                scene.objects[e.objIndex].sel = e.selAfter; // pruned by redo()
            }
            break;
        }
        case Entry::Kind::ObjAdd: {
            SceneObject copy = e.object;
            scene.objects.insert(scene.objects.begin() + e.objIndex, std::move(copy));
            break;
        }
        case Entry::Kind::ObjRemove: {
            if (e.objIndex >= scene.objects.size()) return false;
            e.object = std::move(scene.objects[e.objIndex]);
            scene.objects.erase(scene.objects.begin() + e.objIndex);
            break;
        }
    }
    return true;
}

bool History::undo(Scene& scene) {
    if (undo_.empty()) return false;
    Entry e = std::move(undo_.back());
    undo_.pop_back();
    if (!applyUndoToScene(scene, e)) {
        undo_.push_back(std::move(e)); // restore state; scene inconsistent — should not happen
        return false;
    }
    used_ -= e.bytes;
    redo_.push_back(std::move(e));
    scene.active = std::min(scene.active, static_cast<int>(scene.objects.size()) - 1);
    for (auto& obj : scene.objects) obj.sel.prune(obj.mesh);
    scene.bump();
    lastEntryBytes_ = 0;
    return true;
}

bool History::redo(Scene& scene) {
    if (redo_.empty()) return false;
    Entry e = std::move(redo_.back());
    redo_.pop_back();
    if (!applyRedoToScene(scene, e)) {
        redo_.push_back(std::move(e));
        return false;
    }
    const Entry::Kind kind = e.kind; // captured BEFORE the move below
    const uint32_t objIndex = e.objIndex;
    used_ += e.bytes;
    undo_.push_back(std::move(e));
    if (kind == Entry::Kind::ObjAdd) {
        // The redone state had the (re-)added object ACTIVE — createPrimitive
        // and duplicateActiveObject both activate the new object, so redo
        // restores that exact state (selection/identity consistency).
        scene.active = static_cast<int>(objIndex);
    } else {
        scene.active = std::min(scene.active, static_cast<int>(scene.objects.size()) - 1);
    }
    for (auto& obj : scene.objects) obj.sel.prune(obj.mesh);
    scene.bump();
    lastEntryBytes_ = 0;
    return true;
}

void History::clear() {
    undo_.clear();
    redo_.clear();
    used_ = 0;
    pending_ = 0;
    lastEntryBytes_ = 0;
}

} // namespace blone
