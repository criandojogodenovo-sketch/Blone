package com.blone.app.gl

/**
 * JNI bridge to the C++ core (libblone.so).
 *
 * Threading contract: [surfaceCreated], [surfaceChanged] and [drawFrame] are
 * called from the GLSurfaceView render thread, which owns a current OpenGL ES
 * 3.0 context. [pick] is invoked through `GLSurfaceView.queueEvent` so it also
 * runs on the GL thread; it performs pure CPU math (ray/AABB intersection)
 * using the camera state stored by the last [drawFrame].
 */
object NativeLib {

    init {
        System.loadLibrary("blone")
    }

    /** Compiles shaders, uploads static geometry. Called once per context creation. */
    external fun surfaceCreated()

    /** Updates viewport and projection for the given surface size in pixels. */
    external fun surfaceChanged(width: Int, height: Int)

    /**
     * Renders one frame with the current orbit camera.
     * azimuthDeg/elevationDeg in degrees, distance in world units,
     * (tx, ty, tz) orbit target, selected toggles the selection highlight.
     */
    external fun drawFrame(
        azimuthDeg: Float,
        elevationDeg: Float,
        distance: Float,
        tx: Float,
        ty: Float,
        tz: Float,
        selected: Boolean
    )

    /**
     * Casts a picking ray through pixel (x, y) — coordinates in surface pixels,
     * origin top-left — against the scene objects using the last camera state.
     * Returns true when an object is hit.
     */
    external fun pick(x: Float, y: Float): Boolean
}
