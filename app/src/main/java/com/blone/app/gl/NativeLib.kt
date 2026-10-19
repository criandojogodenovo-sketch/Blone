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
    external fun loopCut(): Int

    // ---- gizmo (correção Fase 2) --------------------------------------------
    /** Selects the gizmo tool shown in edit mode: 1 move, 2 rotate, 3 scale. */
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
