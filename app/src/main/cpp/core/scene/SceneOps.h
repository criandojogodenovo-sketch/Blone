#pragma once
// Scene-level operations: the facade used by JNI. Every mutating function
// follows the Phase 2 safety contract:
//   1. validate (read-only);
//   2. reserve history memory (upper bound) — rejection happens HERE, before
//      any mesh/scene mutation (condition 3);
//   3. mutate;
//   4. post-validate; on failure the pre-state is restored;
//   5. push the exact-cost history entry and remap the selection.

#include "../brush/Brush.h"
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
    // F4-LIMITS (P3): READ-OBJECT sessions transform obj.position ONLY
    // (no half-edge to move, nothing to commit): Move works, Rotate/Scale
    // are rejected with ErrUnsupported at begin, and no history entry is
    // written (read-only transforms are not undoable — documented).
    bool readMode = false;
    Vec3 startPosObj{}; // object.position at session begin (read mode)
};

// ---- scene / object management -------------------------------------------
// Returns the new object index, or a negative ops::Status code.
int createPrimitive(Scene& s, History& h, PrimType type, int segsU, int segsV);
Status deleteActiveObject(Scene& s, History& h);
// Fase 4: adds an imported mesh as a new object through the SAME facade
// contract as createPrimitive: Scene::kMaxObjects respected, history
// reservation BEFORE mutation, exactly ONE ObjectAdded entry (undo removes
// the import, redo restores it), selections pruned and Scene::bump() for
// renderer resync. The object is MOVED in (mesh built by core/io away from
// the scene — the worker never touches the live scene); its position is
// (0,0,0) because imported files already carry world-space coordinates.
Status addImportedObject(Scene& s, History& h, SceneObject&& obj);
// F4-LIMITS (P3): same contract for a READ-ONLY import (geometry in the
// shared immutable RenderMesh; the HalfEdgeMesh stays empty). Requires
// obj.editable == false && obj.readMesh non-empty.
Status addImportedReadObject(Scene& s, History& h, SceneObject&& obj);
// F4-LIMITS (P3): promotion — replaces the read-only object at `index`
// with its EDITABLE version (same name/position). Exactly ONE history
// entry (ObjectSwap): undo restores the read-only object, redo restores
// the editable one. The caller built the editable mesh OFF the scene
// (worker) and already classified it against the profile edit limits.
Status replaceWithEditable(Scene& s, History& h, int index,
                           SceneObject&& replacement);
// Duplicates the ACTIVE object (whole mesh clone, selection NOT copied,
// offset +1.5 X so the copy is visible), activates the copy and pushes an
// ObjectAdded history entry (undo removes the copy). Returns the new index
// or a negative ops::Status code.
int duplicateActiveObject(Scene& s, History& h);
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
// Works in BOTH modes (interface reorganization):
//   - OBJECT mode: the target is ALWAYS the whole active object;
//   - EDIT mode:   the target is the valid selection, or the whole object
//                  when the selection is empty (documented fallback).
Status beginTool(Scene& s, History& h, ToolSession& t, Tool tool, int kind,
                 int axis, float touchX, float touchY);
// touchX/touchY are ABSOLUTE pixel positions (rings need the swept angle).
Status updateTool(Scene& s, ToolSession& t, const GizmoMapping& map,
                  float touchX, float touchY);
Status endTool(Scene& s, History& h, ToolSession& t);
void cancelTool(Scene& s, History& h, ToolSession& t);

// Direct STEP transforms for the properties panel (no drag session):
//   Tool::Move   -> `amount` = world units along the world axis (signed);
//   Tool::Rotate -> `amount` = degrees around the world axis at the pivot;
//   Tool::Scale  -> `amount` = factor (uniform when axis==3, axial otherwise),
//                   clamped to [0.01, 100] like the gizmo path.
// Target: EDIT mode valid selection, otherwise the whole active object.
// Validates finiteness BEFORE mutating; commits a verticesDelta history
// entry when something moved. Reuses the TransformMath helpers — no new math.
Status transformDirect(Scene& s, History& h, Tool tool, int axis, float amount);

// ---- brush tools (Fase 3: Esculpir / Pintar) ------------------------------
// One BRUSH SESSION per gesture — the same granularity contract as the gizmo
// ToolSession: begin reserves the history budget ONCE, move applies the
// brush at the new surface hit, end commits EXACTLY ONE history entry
// (VertDelta for sculpt, ColorDelta for paint) and cancel restores the
// captured start state. The session accumulates the union of affected
// vertices with their start positions/colors, so a long drag remains one
// coherent undo step (never one entry per frame).
enum class BrushTool : int { Sculpt = 0, Paint = 1 };

struct BrushSession {
    bool active = false;
    BrushTool tool = BrushTool::Sculpt;
    int objIndex = -1;
    brush::BrushSettings settings;          // live radius/strength; color/mode/opacity from begin
    std::vector<ElemId> touched;            // sorted unique union of affected vertices
    std::vector<Vec3> startPos;             // parallel to touched (positions at first touch)
    std::vector<Vec3> startColor;           // parallel to touched (colors at first touch)
    bool anyChange = false;                 // true once an application moved/painted something
};

// Begins a stroke on `objIndex` at `hitLocal` (LOCAL space, normal of the
// hit face in `hitNormal`) and applies the first dab. Validates the settings
// and the hit data; reserves the history upper bound for the WHOLE stroke
// (aliveVertexCount-bound) BEFORE mutating. Returns OK, ErrInvalidArg (bad
// tool/settings/hit) or ErrBudget.
Status beginBrushStroke(Scene& s, History& h, BrushSession& b, BrushTool tool,
                        int objIndex, const Vec3& hitLocal,
                        const Vec3& hitNormal,
                        const brush::BrushSettings& settings);
// Continues the stroke at a new surface hit. radius/strength update the
// session settings live (real parameters); no surface hit (finger left the
// mesh) is a no-op that keeps the session valid. Bumps the scene version on
// any change so the renderer resyncs.
Status moveBrushStroke(Scene& s, BrushSession& b, const Vec3& hitLocal,
                       const Vec3& hitNormal, float radius, float strength);
// Ends the stroke: pushes exactly one history entry when something changed,
// releases the reservation otherwise. The session is left inactive.
Status endBrushStroke(Scene& s, History& h, BrushSession& b);
// Cancels the stroke: restores every touched vertex to its start
// position/color, releases the reservation. The session is left inactive.
void cancelBrushStroke(Scene& s, History& h, BrushSession& b);

// ---- history / scene -------------------------------------------------------
Status undo(Scene& s, History& h);
Status redo(Scene& s, History& h);
Status newScene(Scene& s, History& h);

} // namespace blone::sceneops
