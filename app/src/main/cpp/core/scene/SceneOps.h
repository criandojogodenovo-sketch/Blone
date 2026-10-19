#pragma once
// Scene-level operations: the facade used by JNI. Every mutating function
// follows the Phase 2 safety contract:
//   1. validate (read-only);
//   2. reserve history memory (upper bound) — rejection happens HERE, before
//      any mesh/scene mutation (condition 3);
//   3. mutate;
//   4. post-validate; on failure the pre-state is restored;
//   5. push the exact-cost history entry and remap the selection.

#include "../history/History.h"
#include "../mesh/Primitives.h"
#include "../operations/Operations.h"
#include "../operations/TransformMath.h"
#include "Scene.h"

namespace blone::sceneops {

using ops::Status;     // shared status codes
using ops::DragParams; // screen-drag mapping parameters

// Tool ids for the explicit transform controls.
enum class Tool : int { None = 0, Move = 1, Rotate = 2, Scale = 3 };

// Transform tool session (owned by the JNI layer; one at a time).
struct ToolSession {
    bool active = false;
    Tool tool = Tool::None;
    int objIndex = -1;
    std::vector<ElemId> verts;   // deduplicated vertex ids
    std::vector<Vec3> startPos;  // positions at session begin
    DragParams params{};
    float accumDx = 0.0f;
    float accumDy = 0.0f;
    Vec3 pivot{};
};

// ---- scene / object management -------------------------------------------
// Returns the new object index, or a negative ops::Status code.
int createPrimitive(Scene& s, History& h, PrimType type, int segsU, int segsV);
Status deleteActiveObject(Scene& s, History& h);
Status setActiveObject(Scene& s, int index);
Status setEditMode(Scene& s, bool edit);
Status setSelectionMode(Scene& s, SelMode mode);
Status setMultiSelect(Scene& s, bool on);
Status clearSelection(Scene& s);
// kind: 1 vertex, 2 edge, 3 face, 4 object-switch. Applies the multi-select
// policy of the scene.
Status selectFromPick(Scene& s, int kind, int objIndex, ElemId elemId);

// ---- modeling operations ---------------------------------------------------
Status extrude(Scene& s, History& h);
Status subdivide(Scene& s, History& h);
Status deleteSelection(Scene& s, History& h);
Status loopCut(Scene& s, History& h);

// ---- transform tools -------------------------------------------------------
Status beginTool(Scene& s, History& h, ToolSession& t, Tool tool, const DragParams& p);
Status updateTool(Scene& s, ToolSession& t, const DragParams& p, float dxPx, float dyPx);
Status endTool(Scene& s, History& h, ToolSession& t);
void cancelTool(Scene& s, History& h, ToolSession& t);

// ---- history / scene -------------------------------------------------------
Status undo(Scene& s, History& h);
Status redo(Scene& s, History& h);
Status newScene(Scene& s, History& h);

} // namespace blone::sceneops
