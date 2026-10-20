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

using ops::Status; // shared status codes

// Tool ids for the explicit transform controls (mirrors the Kotlin Tool enum).
enum class Tool : int { None = 0, Move = 1, Rotate = 2, Scale = 3 };

// Pixel-space projection of the constrained world axis at the pivot,
// recomputed by the JNI layer on every drag event (camera may differ).
struct GizmoMapping {
    float axisDirX = 1.0f;   // pixel-space direction (y down), normalized
    float axisDirY = 0.0f;
    float pxPerWorld = 1.0f; // apparent pixels per world unit along the axis
    float pivotScreenX = 0.0f;
    float pivotScreenY = 0.0f;
    // World axes sign for ring rotation: +1 when rotating the ring axis
    // towards the camera matches screen-clockwise, -1 otherwise.
    float ringSign = 1.0f;
};

// Transform tool session (owned by the JNI layer; one at a time). Started by
// a GIZMO handle drag (handle hit-test done by the renderer).
struct ToolSession {
    bool active = false;
    Tool tool = Tool::None;
    int objIndex = -1;
    std::vector<ElemId> verts;   // deduplicated vertex ids
    std::vector<Vec3> startPos;  // positions at session begin
    Vec3 pivot{};
    // Gizmo state: handle kinds — 1 axis tip, 2 rotation ring, 3 center.
    int kind = 0;      // gizmo handle kind; 0 = no session started
    int axis = -1;     // 0=X, 1=Y, 2=Z; 3 = uniform (center handle)
    float beginTouchX = 0.0f;
    float beginTouchY = 0.0f;
    float lastTouchX = 0.0f;
    float lastTouchY = 0.0f;
    bool lastTouchValid = false;
    // Accumulated transforms applied to startPos on every update (absolute).
    Vec3 accumMove{};
    float accumFactor = 1.0f;
    float accumDeg = 0.0f;
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

// ---- selection state queries (single source of truth) ---------------------
// True when the ACTIVE object holds at least one ALIVE selected element.
// The renderer gates the gizmo and the Kotlin UI gates its affordances on
// this — an empty/invalid selection never shows transform handles.
bool hasValidSelection(const Scene& s);
// Atomic counts of the ACTIVE object's alive selection [V, E, F]. Ids that
// no longer exist in the mesh are NOT counted.
void selectionCounts(const Scene& s, int out[3]);
// Removes ids that no longer exist from EVERY object's selection and reports
// whether anything changed. Defensive hygiene after any mutation.
bool pruneAllSelections(Scene& s);

// ---- modeling operations ---------------------------------------------------
Status extrude(Scene& s, History& h);
Status subdivide(Scene& s, History& h);
Status deleteSelection(Scene& s, History& h);
Status loopCut(Scene& s, History& h);

// ---- transform tools (gizmo-driven) ---------------------------------------
// begins: tool from the gizmo mode; kind/axis from the renderer hit-test.
Status beginTool(Scene& s, History& h, ToolSession& t, Tool tool, int kind,
                 int axis, float touchX, float touchY);
// touchX/touchY are ABSOLUTE pixel positions (rings need the swept angle).
Status updateTool(Scene& s, ToolSession& t, const GizmoMapping& map,
                  float touchX, float touchY);
Status endTool(Scene& s, History& h, ToolSession& t);
void cancelTool(Scene& s, History& h, ToolSession& t);

// ---- history / scene -------------------------------------------------------
Status undo(Scene& s, History& h);
Status redo(Scene& s, History& h);
Status newScene(Scene& s, History& h);

} // namespace blone::sceneops
