package com.blone.app.gl

import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.sin

/**
 * Pure-Kotlin orbit camera state for the 3D viewport.
 *
 * No Android dependencies — fully unit-testable on the JVM. The UI thread
 * mutates this through gesture handlers; the GL thread reads a snapshot every
 * frame via [BloneViewModel].
 *
 * Conventions (matching the C++ side):
 *  - azimuth rotates around the world Y axis, 0° looks down -Z;
 *  - elevation is clamped to avoid gimbal flip through the poles;
 *  - the camera always orbits around [targetX/Y/Z] which the user can pan.
 */
class CameraState(
    initialAzimuthDeg: Float = 35f,
    initialElevationDeg: Float = 25f,
    initialDistance: Float = 8f,
) {
    var azimuthDeg: Float = initialAzimuthDeg
        private set
    var elevationDeg: Float = initialElevationDeg
        private set
    var distance: Float = initialDistance
        private set

    var targetX: Float = 0f
        private set
    var targetY: Float = 1f
        private set
    var targetZ: Float = 0f
        private set

    /** Drag sensitivity for orbit, in degrees per pixel. */
    var orbitDegreesPerPx: Float = 0.35f

    /** Vertical field of view used for pan world-units-per-pixel math. */
    var fovYDegrees: Float = 45f

    /**
     * Current viewport height in pixels, set by the surface view on layout —
     * anchors the transform-tool drag mapping (same formula as pan).
     */
    @Volatile
    var viewportHeight: Float = 1f

    fun orbit(dxPx: Float, dyPx: Float) {
        azimuthDeg -= dxPx * orbitDegreesPerPx
        elevationDeg += dyPx * orbitDegreesPerPx
        elevationDeg = elevationDeg.coerceIn(MIN_ELEVATION_DEG, MAX_ELEVATION_DEG)
        azimuthDeg = normalizeAngle(azimuthDeg)
    }

    /**
     * Two-finger pan. Moves the orbit target in the camera's screen plane so
     * the scene follows the fingers. [viewportHeightPx] anchors the conversion
     * to the actual surface so pan speed feels identical in both orientations.
     */
    fun pan(dxPx: Float, dyPx: Float, viewportHeightPx: Float) {
        if (viewportHeightPx <= 0f) return
        val worldPerPx = 2f * distance * tanDeg(fovYDegrees * 0.5f) / viewportHeightPx

        val az = Math.toRadians(azimuthDeg.toDouble())
        val el = Math.toRadians(elevationDeg.toDouble())
        val cA = cos(az).toFloat(); val sA = sin(az).toFloat()
        val cE = cos(el).toFloat(); val sE = sin(el).toFloat()

        // Camera right and up axes derived from the orbit direction.
        val rightX = cA; val rightY = 0f; val rightZ = -sA
        val upX = -sA * sE; val upY = cE; val upZ = -cA * sE

        targetX += (-dxPx * rightX + dyPx * upX) * worldPerPx
        targetY += (-dxPx * rightY + dyPx * upY) * worldPerPx
        targetZ += (-dxPx * rightZ + dyPx * upZ) * worldPerPx

        clampTarget()
    }

    /** Pinch zoom: multiplies distance by prevSpan/currentSpan, clamped. */
    fun zoomBySpanRatio(prevSpanPx: Float, currentSpanPx: Float) {
        if (prevSpanPx <= 0f || currentSpanPx <= 0f) return
        distance = (distance * prevSpanPx / currentSpanPx).coerceIn(MIN_DISTANCE, MAX_DISTANCE)
    }

    /** Explicit zoom factor (>1 zooms in), clamped. Used by unit tests and future UI buttons. */
    fun zoomBy(factor: Float) {
        distance = (distance / factor).coerceIn(MIN_DISTANCE, MAX_DISTANCE)
    }

    private fun clampTarget() {
        val dx = targetX
        val dz = targetZ
        val radiusXZ = kotlin.math.sqrt(dx * dx + dz * dz)
        if (radiusXZ > MAX_TARGET_RADIUS_XZ) {
            val k = MAX_TARGET_RADIUS_XZ / radiusXZ
            targetX *= k
            targetZ *= k
        }
        targetY = targetY.coerceIn(MIN_TARGET_Y, MAX_TARGET_Y)
    }

    companion object {
        const val MIN_ELEVATION_DEG = -85f
        const val MAX_ELEVATION_DEG = 85f
        const val MIN_DISTANCE = 2.5f
        const val MAX_DISTANCE = 40f
        const val MAX_TARGET_RADIUS_XZ = 15f
        const val MIN_TARGET_Y = -5f
        const val MAX_TARGET_Y = 8f

        fun normalizeAngle(deg: Float): Float {
            var a = deg
            while (a > 180f) a -= 360f
            while (a < -180f) a += 360f
            return a
        }

        private fun tanDeg(deg: Float): Float {
            val r = Math.toRadians(deg.toDouble())
            return kotlin.math.tan(r).toFloat()
        }
    }
}
