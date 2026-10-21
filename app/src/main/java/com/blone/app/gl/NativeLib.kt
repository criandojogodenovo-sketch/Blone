package com.blone.app.gl

/**
 * JNI bridge to the C++ core (libblone.so).
 *
 * Threading contract: [surfaceCreated], [surfaceChanged] and [drawFrame] are
 * called from the GLSurfaceView render thread, which owns a current OpenGL ES
 * 3.0 context. [pickAndSelect] is invoked through `GLSurfaceView.queueEvent`
 * (also the GL thread). Scene mutations may be called from any thread — the
 * native layer serializes access with a mutex.
 */
object NativeLib {

    /** Status codes returned by editing operations (map in [StatusMessages]). */
    object Status {
        const val OK = 0
        const val ERR_NO_SELECTION = 1
        const val ERR_NO_ACTIVE_OBJECT = 2
        const val ERR_UNSUPPORTED = 3
        const val ERR_NON_MANIFOLD = 4
        const val ERR_BUDGET = 5
        const val ERR_LIMIT = 6
        const val ERR_INTERNAL = 7
        const val ERR_INVALID_ARG = 8
    }

    init {
        System.loadLibrary("blone")
    }

    /** Compiles shaders, uploads static geometry. Called once per context. */
    external fun surfaceCreated()

    /** Updates viewport and projection for the given surface size in pixels. */
    external fun surfaceChanged(width: Int, height: Int)

    /**
     * Reserves the top/bottom pixel bands for the UI chrome so the model is
     * projected and centered into the REALLY visible area. MUST be called on
     * the GL thread (dispatch via GLSurfaceView.queueEvent).
     */
    external fun setViewportInsets(topPx: Int, bottomPx: Int)

    /**
     * Same as [setViewportInsets] with LEFT/RIGHT bands too (side toolbar /
     * property panel). MUST be called on the GL thread. The renderer draws,
     * picks and maps the gizmo inside THIS rect — what you see is what you
     * touch, in every panel state.
     */
    external fun setViewportInsetsRect(topPx: Int, bottomPx: Int, leftPx: Int, rightPx: Int)

    /**
     * Density scale for the gizmo touch targets (DECISÃO D — 44 dp rule):
     * pass `resources.displayMetrics.density` once per process. Hit targets
     * scale 1:1; visual lengths scale moderately (capped natively).
     */
    external fun setGizmoTouchScale(scale: Float)

    /**
     * ATOMIC selection snapshot from the C++ engine (single source of truth):
     * [editMode, selectionMode, multiSelect, hasActiveObject,
     *  nVerts, nEdges, nFaces, hasValidSelection].
     * Dead ids are never counted; hasValidSelection distinguishes an EMPTY
     * selection (all counts zero) from an INVALID one (stale ids dropped).
     */
    external fun selectionSnapshot(): IntArray?

    /** Renders one frame with the given orbit camera and scene state. */
    external fun drawFrame(
        azimuthDeg: Float,
        elevationDeg: Float,
        distance: Float,
        tx: Float,
        ty: Float,
        tz: Float,
        pointRadiusPx: Float,
    )

    /**
     * Tap picking + selection in one atomic call on the GL thread.
     * Returns [kind, objIndex, elemId, nV, nE, nF]; kind 0 = nothing hit,
     * 1 vertex, 2 edge, 3 face, 4 object; negative kind = selection error
     * (value = -status).
     */
    external fun pickAndSelect(xPx: Float, yPx: Float, snapRadiusPx: Float): IntArray

    /** Creates a primitive; returns the new object index or -status. */
    external fun createPrimitive(type: Int, segsU: Int, segsV: Int): Int

    external fun objectCount(): Int

    external fun objectName(index: Int): String?

    /** [vertices, edges, faces, isActive] or null when the index is invalid. */
    external fun objectInfo(index: Int): IntArray?

    /**
     * World-space centroid of the object (whole-object gizmo pivot rule):
     * [x, y, z] or null when the index is invalid / the mesh is empty.
     * Display-only (properties panel readout).
     */
    external fun objectCenter(index: Int): FloatArray?

    external fun setActiveObject(index: Int): Int

    external fun setEditMode(edit: Boolean)

    /** 0 vertex, 1 edge, 2 face. */
    external fun setSelectionMode(mode: Int)

    external fun setMultiSelect(on: Boolean)

    external fun clearSelection()

    // Editing operations — return 0 (OK) or a negative status code.
    external fun extrude(): Int
    external fun subdivide(): Int
    external fun deleteSelection(): Int
    external fun deleteActiveObject(): Int
    external fun duplicateActiveObject(): Int
    external fun loopCut(): Int

    /**
     * Direct STEP transform (properties panel): tool 1 move / 2 rotate /
     * 3 scale; axis 0..2 (3 = uniform, scale only); amount in world units /
     * degrees / factor. Validates finiteness and clamps like the gizmo path;
     * commits one history entry. Returns 0 (OK) or a negative status.
     */
    external fun transformDirect(tool: Int, axis: Int, amount: Float): Int

    // ---- gizmo (correção Fase 2) --------------------------------------------
    /** Selects the gizmo tool: 0 none ("Seleção"), 1 move, 2 rotate, 3 scale. */
    external fun setGizmoTool(tool: Int)

    /**
     * CPU hit-test against the gizmo handles at the given pixel. Returns the
     * handle: 0 none, 1..3 axis tips (X/Y/Z), 4 uniform-scale center,
     * 11..13 rotation rings around X/Y/Z. Cheap (no GL work).
     */
    external fun gizmoHitTest(xPx: Float, yPx: Float): Int

    /** Starts a gizmo drag on [handle]; returns 0 (OK) or -status. */
    external fun beginGizmoDrag(
        handle: Int,
        xPx: Float,
        yPx: Float,
        azimuthDeg: Float,
        elevationDeg: Float,
        distance: Float,
        tx: Float,
        ty: Float,
        tz: Float,
        viewportWidthPx: Float,
        viewportHeightPx: Float,
    ): Int

    /** Continues the gizmo drag with the ABSOLUTE touch pixel position. */
    external fun updateGizmoDrag(
        xPx: Float,
        yPx: Float,
        azimuthDeg: Float,
        elevationDeg: Float,
        distance: Float,
        tx: Float,
        ty: Float,
        tz: Float,
        viewportWidthPx: Float,
        viewportHeightPx: Float,
    ): Int

    /** Ends the drag; commits the history entry when something moved. */
    external fun endGizmoDrag(): Int

    /** Cancels the drag (second finger landed — the camera takes over). */
    external fun cancelGizmoDrag()

    // ---- brush (Fase 3: Esculpir / Pintar) ----------------------------------
    //
    // Same threading contract as the gizmo drag: UI-thread, CPU-only, mutex
    // guarded in native code. The mesh NEVER crosses to Kotlin — hits, the
    // deformation, the painting and the history entry all live in C++; the
    // renderer resyncs from Scene::version on the next frame.

    /**
     * Raw surface hit under the pixel (NO selection side effects). Returns
     * [hit(0/1), objIndex, worldX, worldY, worldZ]; hit 0 = nothing under
     * the finger (no stroke starts).
     */
    external fun brushHit(xPx: Float, yPx: Float): FloatArray?

    /**
     * Starts a stroke on the surface under (xPx,yPx) and applies the first
     * dab. tool: 0 = sculpt, 1 = paint. mode: 0 = raise, 1 = dig (sculpt).
     * r/g/b: paint color in [0,1]. opacity: paint blend in [0.01,1].
     * Returns 0 (OK) or a negative status code.
     */
    external fun beginBrushStroke(
        tool: Int,
        xPx: Float,
        yPx: Float,
        radius: Float,
        strength: Float,
        mode: Int,
        r: Float,
        g: Float,
        b: Float,
        opacity: Float,
    ): Int

    /**
     * Continues the stroke at the current finger position; radius/strength
     * are the LIVE brush parameters. A miss (finger left the mesh) is a
     * valid no-op. Returns 0 (OK) or a negative status code.
     */
    external fun moveBrushStroke(xPx: Float, yPx: Float, radius: Float, strength: Float): Int

    /** Ends the stroke: commits exactly ONE history entry when it changed anything. */
    external fun endBrushStroke(): Int

    /** Cancels the stroke and restores the captured start state. */
    external fun cancelBrushStroke()

    // History.
    external fun canUndo(): Boolean
    external fun canRedo(): Boolean
    external fun historyMemBytes(): Long
    external fun undo(): Int
    external fun redo(): Int

    // Project: BLONEPRJ binary, validated on load.
    external fun saveProject(): ByteArray?
    external fun openProject(bytes: ByteArray): Int

    /** Clears the scene and the history (not undoable, documented). */
    external fun newScene(): Int
}
