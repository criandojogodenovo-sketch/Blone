package com.blone.app.gl

/**
 * Real frame-time based FPS measurement.
 *
 * Frames are timestamped with System.nanoTime() on the GL render thread; the
 * reported value is derived from the actual span of the last [windowSize]
 * frames — nothing is hardcoded or estimated. No Android dependencies, so the
 * math is unit-testable on the JVM.
 */
class FpsMeter(
    private val windowSize: Int = 60,
) {
    private val timestamps = ArrayDeque<Long>(windowSize)

    /** Records one rendered frame. [timestampNanos] must be monotonic (nanoTime). */
    fun onFrame(timestampNanos: Long) {
        timestamps.addLast(timestampNanos)
        while (timestamps.size > windowSize) {
            timestamps.removeFirst()
        }
    }

    /**
     * Average FPS over the current window, or null while fewer than 2 frames
     * were recorded (an honest "not measured yet" instead of a fake value).
     */
    fun averageFps(): Float? {
        if (timestamps.size < 2) return null
        val first = timestamps.first()
        val last = timestamps.last()
        val spanNanos = last - first
        if (spanNanos <= 0) return null
        val frames = (timestamps.size - 1).toFloat()
        return frames / (spanNanos / 1_000_000_000f)
    }

    /** Largest gap between consecutive frames in the window, in milliseconds. */
    fun worstFrameGapMs(): Float? {
        if (timestamps.size < 2) return null
        var worst = 0f
        var prev = timestamps.first()
        for (t in timestamps) {
            val gap = (t - prev) / 1_000_000f
            if (gap > worst) worst = gap
            prev = t
        }
        return worst
    }

    fun measuredFrameCount(): Int = timestamps.size

    fun reset() = timestamps.clear()
}
