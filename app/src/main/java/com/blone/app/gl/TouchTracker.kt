package com.blone.app.gl

/**
 * Platform-free multitouch gesture recognizer for the 3D viewport.
 *
 * The GLSurfaceView translates raw [android.view.MotionEvent]s into the
 * abstract [Pointer] lists accepted here, which keeps this class 100%
 * JVM-testable and free of Android APIs.
 *
 * Gesture policy (correção Fase 2 — gizmo funcional, sem bloqueios):
 *  - one finger drag  -> ORBIT (callback [onOrbit]);
 *  - GIZMO handle hit on touch-down (probe [onGizmoDown] returns a handle
 *    != 0) -> GIZMO drag: the selection/object is transformed along that
 *    handle; the finger NEVER orbits while on the gizmo;
 *  - two fingers      -> PAN (centroid) + ZOOM (span ratio), ALWAYS the
 *    camera — a second finger cancels an active gizmo drag;
 *  - single finger tap (short, below touch slop, never multi-touch) -> [onTap]
 *    (selection picking; a gizmo drag never produces a tap);
 *  - finger added/removed transitions re-anchor references so gestures never
 *    "jump" or get stuck.
 */
class TouchTracker(
    /** Maximum finger travel (px) that still counts as a tap. */
    private val touchSlopPx: Float,
    /** Maximum tap duration (ms). */
    private val tapMaxDurationMs: Long = 300L,
    private val onOrbit: (dxPx: Float, dyPx: Float) -> Unit,
    private val onPanZoom: (dxPx: Float, dyPx: Float, prevSpanPx: Float, currentSpanPx: Float) -> Unit,
    private val onTap: (xPx: Float, yPx: Float) -> Unit,
    /** Probe on ACTION_DOWN; returns the gizmo handle (0 = no handle hit). */
    private val onGizmoDown: (xPx: Float, yPx: Float) -> Int,
    private val onGizmoMove: (xPx: Float, yPx: Float) -> Unit,
    private val onGizmoUp: () -> Unit,
    private val onGizmoCancel: () -> Unit,
) {

    data class Pointer(val id: Int, val x: Float, val y: Float)

    private enum class Mode { IDLE, TAP_CANDIDATE, ORBIT, TWO_FINGER, GIZMO }

    private var mode: Mode = Mode.IDLE

    // Tap candidate bookkeeping
    private var tapStartX = 0f
    private var tapStartY = 0f
    private var tapStartTimeMs = 0L

    // Last-sample bookkeeping for delta computation
    private var lastX = 0f
    private var lastY = 0f
    private var lastCentroidX = 0f
    private var lastCentroidY = 0f
    private var lastSpan = 0f

    fun onDown(pointer: Pointer, timeMs: Long) {
        mode = Mode.TAP_CANDIDATE
        tapStartX = pointer.x
        tapStartY = pointer.y
        tapStartTimeMs = timeMs
        lastX = pointer.x
        lastY = pointer.y
        // A gizmo handle under the finger takes priority: the drag becomes a
        // transform, never an orbit and never a selection tap.
        if (onGizmoDown(pointer.x, pointer.y) != 0) {
            mode = Mode.GIZMO
        }
    }

    fun onPointerDown(pointers: List<Pointer>) {
        if (pointers.size >= 2) {
            // A second finger ALWAYS takes the camera; cancel any gizmo drag
            // and any tap candidate, then start two-finger mode.
            if (mode == Mode.GIZMO) onGizmoCancel()
            mode = Mode.TWO_FINGER
            reanchorTwoFinger(pointers)
        }
    }

    fun onMove(pointers: List<Pointer>) {
        when (mode) {
            Mode.TAP_CANDIDATE, Mode.ORBIT -> {
                val p = pointers.firstOrNull { it.id == primaryId } ?: pointers.firstOrNull()
                    ?: return
                if (mode == Mode.TAP_CANDIDATE &&
                    distance(p.x, p.y, tapStartX, tapStartY) > touchSlopPx
                ) {
                    mode = Mode.ORBIT
                }
                if (mode == Mode.ORBIT) {
                    val dx = p.x - lastX
                    val dy = p.y - lastY
                    if (dx != 0f || dy != 0f) onOrbit(dx, dy)
                }
                lastX = p.x
                lastY = p.y
            }

            Mode.GIZMO -> {
                val p = pointers.firstOrNull { it.id == primaryId } ?: pointers.firstOrNull()
                    ?: return
                onGizmoMove(p.x, p.y)
            }

            Mode.TWO_FINGER -> {
                val a = pointers.getOrNull(0) ?: return
                val b = pointers.getOrNull(1) ?: return
                val centroidX = (a.x + b.x) * 0.5f
                val centroidY = (a.y + b.y) * 0.5f
                val span = distance(a.x, a.y, b.x, b.y)

                val dx = centroidX - lastCentroidX
                val dy = centroidY - lastCentroidY
                if (dx != 0f || dy != 0f || lastSpan <= 0f || span != lastSpan) {
                    onPanZoom(dx, dy, lastSpan, span)
                }
                lastCentroidX = centroidX
                lastCentroidY = centroidY
                lastSpan = span
            }

            Mode.IDLE -> Unit
        }
    }

    /**
     * One of two fingers was lifted. If one finger remains we re-anchor orbit
     * tracking to it (or keep two-finger mode if still >= 2 pointers), so the
     * gesture continues without a jump.
     */
    fun onPointerUp(pointers: List<Pointer>) {
        when {
            pointers.size >= 2 -> {
                mode = Mode.TWO_FINGER
                reanchorTwoFinger(pointers)
            }

            pointers.size == 1 -> {
                mode = Mode.ORBIT
                val p = pointers[0]
                lastX = p.x
                lastY = p.y
            }

            else -> mode = Mode.IDLE
        }
    }

    fun onUp(pointer: Pointer, timeMs: Long) {
        if (mode == Mode.GIZMO) {
            onGizmoUp()
        } else {
            val wasTap = mode == Mode.TAP_CANDIDATE &&
                distance(pointer.x, pointer.y, tapStartX, tapStartY) <= touchSlopPx &&
                (timeMs - tapStartTimeMs) <= tapMaxDurationMs
            if (wasTap) onTap(pointer.x, pointer.y)
        }
        mode = Mode.IDLE
    }

    fun onCancel() {
        if (mode == Mode.GIZMO) onGizmoCancel()
        mode = Mode.IDLE
    }

    private var primaryId: Int = -1

    private fun reanchorTwoFinger(pointers: List<Pointer>) {
        val a = pointers[0]
        val b = pointers[1]
        if (primaryId == -1) primaryId = a.id
        lastCentroidX = (a.x + b.x) * 0.5f
        lastCentroidY = (a.y + b.y) * 0.5f
        lastSpan = distance(a.x, a.y, b.x, b.y)
    }

    private fun distance(x1: Float, y1: Float, x2: Float, y2: Float): Float {
        val dx = x1 - x2
        val dy = y1 - y2
        return kotlin.math.sqrt(dx * dx + dy * dy)
    }
}
