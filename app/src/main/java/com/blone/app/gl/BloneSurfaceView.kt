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
 * handling for the viewport gestures.
 *
 * Lifecycle: callers MUST invoke [onPause]/[onResume] (inherited from
 * GLSurfaceView) with the Activity lifecycle; [com.blone.app.MainActivity]
 * wires that through a LifecycleObserver.
 */
class BloneSurfaceView(
    context: Context,
    private val viewModel: BloneViewModel,
) : GLSurfaceView(context) {

    private val density = context.resources.displayMetrics.density
    private val touchSlopPx = 10f * density

    private val fpsMeter = FpsMeter(windowSize = 60)
    private var lastFpsPublishNanos = 0L

    private val tracker = TouchTracker(
        touchSlopPx = touchSlopPx,
        onOrbit = { dx, dy ->
            post {
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
    )

    init {
        setEGLContextClientVersion(3)
        // 24-bit depth buffer, no stencil — enough for this scene.
        setEGLConfigChooser(8, 8, 8, 8, 24, 0)
        preserveEGLContextOnPause = true
        setRenderer(RendererBridge())
        renderMode = RENDERMODE_CONTINUOUSLY
    }

    /** Sends the current camera snapshot to native code (called each frame). */
    private fun pushCameraAndDraw(selected: Boolean) {
        val cam = viewModel.camera
        NativeLib.drawFrame(
            azimuthDeg = cam.azimuthDeg,
            elevationDeg = cam.elevationDeg,
            distance = cam.distance,
            tx = cam.targetX,
            ty = cam.targetY,
            tz = cam.targetZ,
            selected = selected,
        )
    }

    /** Runs picking on the GL thread, result delivered on the UI thread. */
    fun performPick(xPx: Float, yPx: Float) {
        queueEvent {
            val hit = try {
                NativeLib.pick(xPx, yPx)
            } catch (t: Throwable) {
                false
            }
            post { viewModel.onPickResult(hit) }
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
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
            NativeLib.surfaceChanged(width, height)
        }

        override fun onDrawFrame(gl: GL10?) {
            val frameNanos = System.nanoTime()
            pushCameraAndDraw(viewModel.selected)
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
