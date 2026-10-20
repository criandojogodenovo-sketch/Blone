package com.blone.app.gl

import android.annotation.SuppressLint
import android.content.Context
import android.opengl.GLSurfaceView
import android.view.MotionEvent
import com.blone.app.vm.BloneViewModel
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * The OpenGL ES 3.0 surface that hosts the C++ renderer, plus raw multitouch
 * handling (correção Fase 2 — gizmo funcional, sem bloqueios):
 *  - Compose buttons consume their own touches (they sit above this view);
 *  - touch-down on a GIZMO handle: drag transforms the selection/object;
 *  - 1 finger anywhere else: camera orbit;
 *  - 2 fingers: ALWAYS the camera (pan + zoom), cancelling a gizmo drag;
 *  - short single-finger tap (not on the gizmo): element/object picking.
 */
class BloneSurfaceView(
    context: Context,
    private val viewModel: BloneViewModel,
) : GLSurfaceView(context) {

    private val density = context.resources.displayMetrics.density
    private val touchSlopPx = 10f * density
    private val pickRadiusPx = 14f * density

    private val fpsMeter = FpsMeter(windowSize = 60)
    private var lastFpsPublishNanos = 0L

    private val tracker = TouchTracker(
        touchSlopPx = touchSlopPx,
        onOrbit = { dx, dy ->
            post {
                // 1 finger outside the gizmo is ALWAYS the camera orbit —
                // the gizmo drag owns its own gesture path.
                viewModel.camera.orbit(dx, dy)
            }
        },
        onPanZoom = { dx, dy, prevSpan, curSpan ->
            post {
                viewModel.camera.pan(dx, dy, height.toFloat().coerceAtLeast(1f))
                viewModel.camera.zoomBySpanRatio(prevSpan, curSpan)
            }
        },
        onTap = { x, y ->
            performPick(x, y)
        },
        onGizmoDown = { x, y ->
            // Runs on the UI thread; the native hit-test is CPU-only and
            // mutex-guarded (one frame of contention at most).
            // NO mode gate here: the ENGINE decides in both modes — the
            // object-mode gizmo (whole active object) and the edit-mode
            // gizmo (valid selection) share the same gate as the drawing.
            val handle = try {
                NativeLib.gizmoHitTest(x, y)
            } catch (_: Throwable) {
                0
            }
            if (handle != 0) {
                viewModel.beginGizmoDrag(handle, x, y)
            }
            handle
        },
        onGizmoMove = { x, y ->
            viewModel.onGizmoDragMove(x, y)
        },
        onGizmoUp = {
            viewModel.onGizmoDragEnd()
        },
        onGizmoCancel = {
            viewModel.onGizmoDragCancel()
        },
        // Fase 3: brush stroke path (sculpt/paint modes only). The probe
        // returns false when nothing is under the finger -> the gesture
        // falls through to the camera orbit/tap path.
        onBrushDown = { x, y ->
            viewModel.beginBrushAt(x, y)
        },
        onBrushMove = { x, y ->
            viewModel.onBrushMove(x, y)
        },
        onBrushUp = {
            viewModel.onBrushEnd()
        },
        onBrushCancel = {
            viewModel.onBrushCancel()
        },
    )

    init {
        setEGLContextClientVersion(3)
        setEGLConfigChooser(8, 8, 8, 8, 24, 0)
        preserveEGLContextOnPause = true
        setRenderer(RendererBridge())
        renderMode = RENDERMODE_CONTINUOUSLY
        // DECISÃO D: gizmo touch targets scale with density (hit targets 1:1,
        // visual lengths capped natively). Not GL state — safe pre-context.
        try {
            NativeLib.setGizmoTouchScale(density)
        } catch (_: Throwable) {
            // Native not ready — the renderer default (1.0) applies.
        }
    }

    /** Sends the current camera snapshot to native code (called each frame). */
    private fun pushCameraAndDraw(pointRadiusPx: Float) {
        val cam = viewModel.camera
        NativeLib.drawFrame(
            azimuthDeg = cam.azimuthDeg,
            elevationDeg = cam.elevationDeg,
            distance = cam.distance,
            tx = cam.targetX,
            ty = cam.targetY,
            tz = cam.targetZ,
            pointRadiusPx = pointRadiusPx,
        )
    }

    /** Runs picking + selection on the GL thread; result to the UI thread. */
    fun performPick(xPx: Float, yPx: Float) {
        queueEvent {
            val res = try {
                NativeLib.pickAndSelect(xPx, yPx, pickRadiusPx)
            } catch (_: Throwable) {
                null
            }
            post { viewModel.onPickResult(res) }
        }
    }

    /**
     * Reserves the top/bottom pixel bands for the Compose chrome so the model
     * is projected into the REALLY visible area. The GL call runs on the GL
     * thread (queueEvent) — never off-context.
     */
    fun applyViewportInsets(topPx: Int, bottomPx: Int) {
        queueEvent {
            try {
                NativeLib.setViewportInsets(topPx, bottomPx)
            } catch (_: Throwable) {
                // Context tearing down — the next surfaceChanged re-applies.
            }
            requestRender()
        }
    }

    /**
     * Same, including LEFT/RIGHT bands (side toolbar / property panel). The
     * renderer draws, picks and maps the gizmo inside THIS rect.
     */
    fun applyViewportRect(topPx: Int, bottomPx: Int, leftPx: Int, rightPx: Int) {
        queueEvent {
            try {
                NativeLib.setViewportInsetsRect(topPx, bottomPx, leftPx, rightPx)
            } catch (_: Throwable) {
                // Context tearing down — the next surfaceChanged re-applies.
            }
            requestRender()
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        // Brush probes only in the sculpt/paint modes (read per event — the
        // mode can change between gestures, never mid-gesture).
        tracker.brushProbeEnabled = viewModel.brushModeActive
        val active = (0 until event.pointerCount).map { i ->
            TouchTracker.Pointer(event.getPointerId(i), event.getX(i), event.getY(i))
        }
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> tracker.onDown(active[0], event.downTime)
            MotionEvent.ACTION_POINTER_DOWN -> tracker.onPointerDown(active)
            MotionEvent.ACTION_MOVE -> tracker.onMove(active)
            MotionEvent.ACTION_POINTER_UP -> {
                val upId = event.getPointerId(event.actionIndex)
                tracker.onPointerUp(active.filter { it.id != upId })
            }
            MotionEvent.ACTION_UP -> tracker.onUp(active[0], event.eventTime)
            MotionEvent.ACTION_CANCEL -> tracker.onCancel()
        }
        return true
    }

    private inner class RendererBridge : Renderer {

        override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
            NativeLib.surfaceCreated()
        }

        override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
            fpsMeter.reset()
            viewModel.camera.viewportHeight = height.toFloat().coerceAtLeast(1f)
            viewModel.camera.viewportWidth = width.toFloat().coerceAtLeast(1f)
            NativeLib.surfaceChanged(width, height)
        }

        override fun onDrawFrame(gl: GL10?) {
            val frameNanos = System.nanoTime()
            pushCameraAndDraw(14f * density)
            fpsMeter.onFrame(frameNanos)

            // Publish real measured FPS at most ~2x per second.
            if (frameNanos - lastFpsPublishNanos >= 500_000_000L) {
                lastFpsPublishNanos = frameNanos
                val avg = fpsMeter.averageFps()
                if (avg != null) {
                    val text = "%,.0f FPS".format(avg)
                    post { viewModel.fpsText = text }
                }
            }
        }
    }
}
