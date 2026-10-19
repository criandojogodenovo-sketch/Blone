package com.blone.app

import com.blone.app.gl.CameraState
import com.blone.app.gl.FpsMeter
import com.blone.app.gl.TouchTracker
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

class CameraStateTest {

    @Test
    fun `elevation is clamped to avoid pole flip`() {
        val cam = CameraState()
        cam.orbit(0f, -10_000f)
        assertTrue(cam.elevationDeg >= CameraState.MIN_ELEVATION_DEG)
        cam.orbit(0f, 10_000f)
        assertTrue(cam.elevationDeg <= CameraState.MAX_ELEVATION_DEG)
    }

    @Test
    fun `distance stays within safe bounds on zoom`() {
        val cam = CameraState(initialDistance = 8f)
        cam.zoomBySpanRatio(1f, 10_000f) // zoom in hard
        assertEquals(CameraState.MIN_DISTANCE, cam.distance, 0.001f)
        cam.zoomBySpanRatio(10_000f, 1f) // zoom out hard
        assertEquals(CameraState.MAX_DISTANCE, cam.distance, 0.001f)
    }

    @Test
    fun `orbit drag right decreases azimuth`() {
        val cam = CameraState(initialAzimuthDeg = 0f)
        cam.orbit(100f, 0f)
        assertTrue("expected negative azimuth, got ${cam.azimuthDeg}", cam.azimuthDeg < 0f)
    }

    @Test
    fun `orbit drag up increases elevation`() {
        val cam = CameraState(initialElevationDeg = 10f)
        cam.orbit(0f, 50f)
        assertTrue(cam.elevationDeg > 10f)
    }

    @Test
    fun `pan with one finger drag right moves target left`() {
        val cam = CameraState(initialAzimuthDeg = 0f, initialElevationDeg = 0f)
        val beforeX = cam.targetX
        cam.pan(200f, 0f, viewportHeightPx = 1000f)
        assertTrue("targetX should decrease, got ${cam.targetX}", cam.targetX < beforeX)
    }

    @Test
    fun `pan vertical moves target along camera up`() {
        val cam = CameraState(initialAzimuthDeg = 0f, initialElevationDeg = 0f)
        val beforeY = cam.targetY
        cam.pan(0f, 200f, viewportHeightPx = 1000f)
        assertTrue(cam.targetY > beforeY)
    }

    @Test
    fun `target is clamped to a reasonable area`() {
        val cam = CameraState()
        repeat(50) { cam.pan(1000f, 1000f, 800f) }
        val radius = kotlin.math.sqrt(cam.targetX * cam.targetX + cam.targetZ * cam.targetZ)
        assertTrue(radius <= CameraState.MAX_TARGET_RADIUS_XZ + 0.01f)
        assertTrue(cam.targetY in CameraState.MIN_TARGET_Y..CameraState.MAX_TARGET_Y)
    }

    @Test
    fun `normalizeAngle wraps into -180 to 180`() {
        assertEquals(10f, CameraState.normalizeAngle(370f), 0.001f)
        assertEquals(-10f, CameraState.normalizeAngle(-370f), 0.001f)
        assertEquals(0f, CameraState.normalizeAngle(720f), 0.001f)
    }
}

class TouchTrackerTest {

    private class Recorder {
        var orbitCalls = 0
        var lastOrbitDx = 0f
        var lastOrbitDy = 0f
        var panZoomCalls = 0
        var lastPanDx = 0f
        var lastPanDy = 0f
        var lastPrevSpan = 0f
        var lastCurSpan = 0f
        var taps = 0
        var lastTapX = 0f
        var lastTapY = 0f
        var gizmoDownCalls = 0
        var gizmoHandle = 0
        var gizmoMoves = 0
        var gizmoUps = 0
        var gizmoCancels = 0
    }

    private fun trackerWith(rec: Recorder) = TouchTracker(
        touchSlopPx = 8f,
        onOrbit = { dx, dy -> rec.orbitCalls++; rec.lastOrbitDx = dx; rec.lastOrbitDy = dy },
        onPanZoom = { dx, dy, prev, cur ->
            rec.panZoomCalls++; rec.lastPanDx = dx; rec.lastPanDy = dy
            rec.lastPrevSpan = prev; rec.lastCurSpan = cur
        },
        onTap = { x, y -> rec.taps++; rec.lastTapX = x; rec.lastTapY = y },
        onGizmoDown = { x, y ->
            rec.gizmoDownCalls++
            rec.gizmoHandle
        },
        onGizmoMove = { _, _ -> rec.gizmoMoves++ },
        onGizmoUp = { rec.gizmoUps++ },
        onGizmoCancel = { rec.gizmoCancels++ },
    )

    @Test
    fun `short single finger tap is detected`() {
        val rec = Recorder()
        val t = trackerWith(rec)
        t.onDown(TouchTracker.Pointer(0, 100f, 100f), 0L)
        t.onMove(listOf(TouchTracker.Pointer(0, 103f, 102f)))
        t.onUp(TouchTracker.Pointer(0, 103f, 102f), 120L)
        assertEquals(1, rec.taps)
        assertEquals(103f, rec.lastTapX, 0.001f)
    }

    @Test
    fun `drag beyond slop becomes orbit and cancels tap`() {
        val rec = Recorder()
        val t = trackerWith(rec)
        t.onDown(TouchTracker.Pointer(0, 100f, 100f), 0L)
        t.onMove(listOf(TouchTracker.Pointer(0, 140f, 100f)))
        t.onUp(TouchTracker.Pointer(0, 140f, 100f), 250L)
        assertEquals(0, rec.taps)
        assertTrue(rec.orbitCalls > 0)
        assertEquals(40f, rec.lastOrbitDx, 0.001f)
    }

    @Test
    fun `slow press is not a tap`() {
        val rec = Recorder()
        val t = trackerWith(rec)
        t.onDown(TouchTracker.Pointer(0, 100f, 100f), 0L)
        t.onUp(TouchTracker.Pointer(0, 100f, 100f), 900L)
        assertEquals(0, rec.taps)
    }

    @Test
    fun `two finger pinch reports span ratio and centroid pan`() {
        val rec = Recorder()
        val t = trackerWith(rec)
        t.onDown(TouchTracker.Pointer(0, 100f, 500f), 0L)
        t.onPointerDown(listOf(TouchTracker.Pointer(0, 100f, 500f), TouchTracker.Pointer(1, 300f, 500f)))
        t.onMove(listOf(TouchTracker.Pointer(0, 50f, 500f), TouchTracker.Pointer(1, 350f, 500f)))
        assertEquals(1, rec.panZoomCalls)
        // centroid moved from (200,500) to (200,500): no pan, pure zoom, span 200->300
        assertEquals(200f, rec.lastPrevSpan, 0.001f)
        assertEquals(300f, rec.lastCurSpan, 0.001f)
    }

    @Test
    fun `two finger pan reports centroid movement`() {
        val rec = Recorder()
        val t = trackerWith(rec)
        t.onDown(TouchTracker.Pointer(0, 100f, 500f), 0L)
        t.onPointerDown(listOf(TouchTracker.Pointer(0, 100f, 500f), TouchTracker.Pointer(1, 300f, 500f)))
        t.onMove(listOf(TouchTracker.Pointer(0, 120f, 520f), TouchTracker.Pointer(1, 320f, 520f)))
        assertEquals(20f, rec.lastPanDx, 0.001f)
        assertEquals(20f, rec.lastPanDy, 0.001f)
    }

    @Test
    fun `releasing one of two fingers re-anchors orbit without jump`() {
        val rec = Recorder()
        val t = trackerWith(rec)
        t.onDown(TouchTracker.Pointer(0, 100f, 500f), 0L)
        t.onPointerDown(listOf(TouchTracker.Pointer(0, 100f, 500f), TouchTracker.Pointer(1, 300f, 500f)))
        t.onMove(listOf(TouchTracker.Pointer(0, 110f, 510f), TouchTracker.Pointer(1, 310f, 510f)))
        rec.orbitCalls = 0

        // Finger 1 lifted; finger 0 remains at (110, 510).
        t.onPointerUp(listOf(TouchTracker.Pointer(0, 110f, 510f)))
        t.onMove(listOf(TouchTracker.Pointer(0, 120f, 520f)))
        t.onUp(TouchTracker.Pointer(0, 120f, 520f), 1000L)

        // The first orbit delta after re-anchoring must be exactly the new movement.
        assertEquals(1, rec.orbitCalls)
        assertEquals(10f, rec.lastOrbitDx, 0.001f)
        assertEquals(10f, rec.lastOrbitDy, 0.001f)
        assertEquals(0, rec.taps) // no phantom tap after multi-touch
    }

    @Test
    fun `cancel clears state and emits nothing`() {
        val rec = Recorder()
        val t = trackerWith(rec)
        t.onDown(TouchTracker.Pointer(0, 100f, 100f), 0L)
        t.onCancel()
        t.onUp(TouchTracker.Pointer(0, 100f, 100f), 50L)
        assertEquals(0, rec.taps)
        assertEquals(0, rec.orbitCalls)
    }

    @Test
    fun `gizmo hit on down routes moves to the gizmo, never orbit or tap`() {
        val rec = Recorder()
        rec.gizmoHandle = 1 // X axis tip under the finger
        val t = trackerWith(rec)
        t.onDown(TouchTracker.Pointer(0, 100f, 100f), 0L)
        t.onMove(listOf(TouchTracker.Pointer(0, 160f, 100f)))
        t.onMove(listOf(TouchTracker.Pointer(0, 180f, 100f)))
        t.onUp(TouchTracker.Pointer(0, 180f, 100f), 400L)
        assertEquals(1, rec.gizmoDownCalls)
        assertEquals(2, rec.gizmoMoves)
        assertEquals(1, rec.gizmoUps)
        assertEquals(0, rec.orbitCalls)
        assertEquals(0, rec.taps) // a gizmo drag never selects
    }

    @Test
    fun `gizmo miss keeps the normal orbit and tap path`() {
        val rec = Recorder()
        rec.gizmoHandle = 0 // nothing under the finger
        val t = trackerWith(rec)
        t.onDown(TouchTracker.Pointer(0, 100f, 100f), 0L)
        t.onMove(listOf(TouchTracker.Pointer(0, 150f, 100f)))
        t.onUp(TouchTracker.Pointer(0, 150f, 100f), 200L)
        assertEquals(1, rec.gizmoDownCalls)
        assertEquals(0, rec.gizmoMoves)
        assertTrue(rec.orbitCalls > 0)
        assertEquals(0, rec.taps) // it was a drag, not a tap
    }

    @Test
    fun `second finger cancels gizmo and takes the camera`() {
        val rec = Recorder()
        rec.gizmoHandle = 2
        val t = trackerWith(rec)
        t.onDown(TouchTracker.Pointer(0, 100f, 500f), 0L)
        assertEquals(1, rec.gizmoDownCalls)
        t.onPointerDown(listOf(TouchTracker.Pointer(0, 100f, 500f), TouchTracker.Pointer(1, 300f, 500f)))
        assertEquals(1, rec.gizmoCancels)
        t.onMove(listOf(TouchTracker.Pointer(0, 120f, 500f), TouchTracker.Pointer(1, 320f, 500f)))
        assertEquals(1, rec.panZoomCalls) // camera, not gizmo
        assertEquals(0, rec.gizmoMoves)
        t.onUp(TouchTracker.Pointer(0, 120f, 500f), 300L)
        assertEquals(0, rec.gizmoUps) // already cancelled, no double end
    }

    @Test
    fun `tap on gizmo handle is silent (no accidental selection)`() {
        val rec = Recorder()
        rec.gizmoHandle = 3
        val t = trackerWith(rec)
        t.onDown(TouchTracker.Pointer(0, 100f, 100f), 0L)
        t.onUp(TouchTracker.Pointer(0, 100f, 100f), 80L)
        assertEquals(1, rec.gizmoUps)
        assertEquals(0, rec.taps)
        assertEquals(0, rec.gizmoMoves) // no movement happened
    }
}

class FpsMeterTest {

    private fun timestampsAt60Fps(count: Int): List<Long> {
        val frameNanos = 1_000_000_000L / 60L
        return List(count) { it.toLong() * frameNanos }
    }

    @Test
    fun `reports null before two frames are measured`() {
        val meter = FpsMeter()
        assertNull(meter.averageFps())
        meter.onFrame(0L)
        assertNull(meter.averageFps())
    }

    @Test
    fun `measures 60fps from real timestamps`() {
        val meter = FpsMeter()
        timestampsAt60Fps(60).forEach { meter.onFrame(it) }
        val fps = meter.averageFps()
        assertNotNull(fps)
        assertEquals(60f, fps!!, 0.5f)
    }

    @Test
    fun `measures 30fps from real timestamps`() {
        val meter = FpsMeter()
        val frameNanos = 1_000_000_000L / 30L
        repeat(30) { meter.onFrame(it.toLong() * frameNanos) }
        assertEquals(30f, meter.averageFps()!!, 0.5f)
    }

    @Test
    fun `window is bounded`() {
        val meter = FpsMeter(windowSize = 10)
        timestampsAt60Fps(100).forEach { meter.onFrame(it) }
        assertEquals(10, meter.measuredFrameCount())
    }

    @Test
    fun `reset clears measurements`() {
        val meter = FpsMeter()
        timestampsAt60Fps(30).forEach { meter.onFrame(it) }
        meter.reset()
        assertNull(meter.averageFps())
    }

    @Test
    fun `worst frame gap is detected`() {
        val meter = FpsMeter()
        meter.onFrame(0L)
        meter.onFrame(16_666_666L)
        meter.onFrame(200_000_000L) // big stall
        meter.onFrame(216_666_666L)
        assertTrue(meter.worstFrameGapMs()!! > 150f)
    }
}
