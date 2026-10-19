#pragma once
// Scene: the list of editable objects plus UI-relevant editing state.
// Deliberately small — no generic framework, only what Phase 2 needs.

#include "../mesh/HalfEdgeMesh.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace blone {

// Selection mode exposed to the UI.
enum class SelMode : int { Vertex = 0, Edge = 1, Face = 2 };

struct Selection {
    using Set = std::set<ElemId>;
    Set verts;
    Set edges;
    Set faces;

    void clear() {
        verts.clear();
        edges.clear();
        faces.clear();
    }
    bool empty() const { return verts.empty() && edges.empty() && faces.empty(); }

    // Removes ids that are no longer alive in the mesh (checked per type:
    // id ranges of vertices/edges/faces are independent counters).
    void prune(const HalfEdgeMesh& m) {
        for (auto it = verts.begin(); it != verts.end();) {
            if (!m.vertexAlive(*it)) it = verts.erase(it); else ++it;
        }
        for (auto it = edges.begin(); it != edges.end();) {
            if (!m.edgeAlive(*it)) it = edges.erase(it); else ++it;
        }
        for (auto it = faces.begin(); it != faces.end();) {
            if (!m.faceAlive(*it)) it = faces.erase(it); else ++it;
        }
    }
};

struct SceneObject {
    std::string name;
    Vec3 position{0.0f, 0.0f, 0.0f}; // object transform (translation, Phase 2)
    HalfEdgeMesh mesh;
    Selection sel;
};

class Scene {
public:
    std::vector<SceneObject> objects;
    int active = -1;
    bool editMode = false;
    SelMode selectionMode = SelMode::Face;
    bool multiSelect = false;
    uint32_t version = 1; // bumped on every structural change (renderer resync)

    static constexpr size_t kMaxObjects = 32;

    SceneObject* activeObject() {
        if (active < 0 || active >= static_cast<int>(objects.size())) return nullptr;
        return &objects[static_cast<size_t>(active)];
    }
    const SceneObject* activeObject() const {
        if (active < 0 || active >= static_cast<int>(objects.size())) return nullptr;
        return &objects[static_cast<size_t>(active)];
    }
    void bump() { ++version; }

    void clearScene() {
        objects.clear();
        active = -1;
        editMode = false;
        selectionMode = SelMode::Face;
        multiSelect = false;
        bump();
    }
};

} // namespace blone
