// Fase 3 — brush gesture tests for the pure-Kotlin TouchTracker (JVM, no
// Android APIs): the brush probe takes the gesture when enabled and the
// finger is over the surface; the camera always wins with a second finger;
// a disabled probe never produces strokes.
package com.blone.app

import com.blone.app.gl.TouchTracker
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class BrushGestureTests {

    private class Recorder {
        var orbitCalls = 0
        var taps = 0
        var brushDownCalls = 0
        var brushDownResult = true
        var brushMoves = 0
        var brushUps = 0
        var brushCancels = 0
        var panZoomCalls = 0

        fun tracker(): TouchTracker = TouchTracker(
            touchSlopPx = 8f,
            onOrbit = { _, _ -> orbitCalls++ },
            onPanZoom = { _, _, _, _ -> panZoomCalls++ },
            onTap = { _, _ -> taps++ },
            onGizmoDown = { _, _ -> 0 },
            onGizmoMove = { _, _ -> },
            onGizmoUp = {},
            onGizmoCancel = {},
            onBrushDown = { _, _ ->
                brushDownCalls++
                brushDownResult
            },
            onBrushMove = { _, _ -> brushMoves++ },
            onBrushUp = { brushUps++ },
            onBrushCancel = { brushCancels++ },
        )
    }

    @Test
    fun `enabled probe with surface hit starts a stroke`() {
        val rec = Recorder()
        val t = rec.tracker()
        t.brushProbeEnabled = true
        t.onDown(TouchTracker.Pointer(0, 100f, 100f), 0L)
        assertEquals(1, rec.brushDownCalls)
        t.onMove(listOf(TouchTracker.Pointer(0, 130f, 110f)))
        t.onMove(listOf(TouchTracker.Pointer(0, 150f, 120f)))
        t.onUp(TouchTracker.Pointer(0, 150f, 120f), 400L)
        assertEquals(2, rec.brushMoves)
        assertEquals(1, rec.brushUps)
        assertEquals(0, rec.orbitCalls) // a stroke NEVER orbits
        assertEquals(0, rec.taps)       // nor produces a selection tap
    }

    @Test
    fun `probe miss falls through to orbit`() {
        val rec = Recorder()
        val t = rec.tracker()
        t.brushProbeEnabled = true
        rec.brushDownResult = false // no surface under the finger
        t.onDown(TouchTracker.Pointer(0, 100f, 100f), 0L)
        assertEquals(1, rec.brushDownCalls)
        t.onMove(listOf(TouchTracker.Pointer(0, 140f, 100f)))
        t.onUp(TouchTracker.Pointer(0, 140f, 100f), 250L)
        assertTrue(rec.orbitCalls > 0)  // camera took over
        assertEquals(0, rec.brushMoves) // no stroke
        assertEquals(0, rec.brushUps)
    }

    @Test
    fun `disabled probe never strokes (selection modes)`() {
        val rec = Recorder()
        val t = rec.tracker()
        t.brushProbeEnabled = false // Objeto/Editar modes
        t.onDown(TouchTracker.Pointer(0, 100f, 100f), 0L)
        assertEquals(0, rec.brushDownCalls) // probe not even consulted
        t.onMove(listOf(TouchTracker.Pointer(0, 140f, 100f)))
        t.onUp(TouchTracker.Pointer(0, 140f, 100f), 250L)
        assertTrue(rec.orbitCalls > 0)
        assertEquals(0, rec.brushMoves)
    }

    @Test
    fun `second finger cancels the stroke and takes the camera`() {
        val rec = Recorder()
        val t = rec.tracker()
        t.brushProbeEnabled = true
        t.onDown(TouchTracker.Pointer(0, 100f, 400f), 0L)
        assertEquals(1, rec.brushDownCalls)
        t.onPointerDown(
            listOf(
                TouchTracker.Pointer(0, 100f, 400f),
                TouchTracker.Pointer(1, 300f, 400f),
            )
        )
        assertEquals(1, rec.brushCancels) // stroke cancelled
        t.onMove(
            listOf(
                TouchTracker.Pointer(0, 120f, 400f),
                TouchTracker.Pointer(1, 320f, 400f),
            )
        )
        assertEquals(1, rec.panZoomCalls) // camera owns the gesture
        assertEquals(0, rec.brushMoves)   // the stroke is dead
        t.onUp(TouchTracker.Pointer(0, 120f, 400f), 300L)
        assertEquals(0, rec.brushUps)     // no stroke end after cancel
    }

    @Test
    fun `stroke up after a real drag commits once`() {
        val rec = Recorder()
        val t = rec.tracker()
        t.brushProbeEnabled = true
        t.onDown(TouchTracker.Pointer(0, 100f, 100f), 0L)
        t.onMove(listOf(TouchTracker.Pointer(0, 110f, 100f)))
        t.onMove(listOf(TouchTracker.Pointer(0, 120f, 100f)))
        t.onUp(TouchTracker.Pointer(0, 120f, 100f), 500L)
        assertEquals(1, rec.brushUps)
        assertEquals(0, rec.brushCancels)
    }
}
