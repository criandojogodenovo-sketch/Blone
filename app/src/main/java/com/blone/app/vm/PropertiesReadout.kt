package com.blone.app.vm

/**
 * Pure, JVM-testable mapping of the engine readout to the properties panel
 * state (hotfix D3). The ViewModel feeds it with the ENGINE's answers and
 * this class decides what the panel shows — no native calls, no Android
 * imports, so the behavior is pinned by unit tests:
 *
 *  - active index inside [0, objectCount): real label, real V/A/F counts and
 *    the real world-space center — including the import-drain case, where
 *    the freshly drained object becomes the active one;
 *  - active index outside the range (no object / stale index): the empty
 *    readout ("" label, all zeros, null center) — the panel shows its
 *    "Nenhum objeto" state.
 *
 * The label composes the engine object name with the 1-based position
 * ("Cubo 1"); a missing/empty engine name falls back to "Objeto <n>".
 */
object PropertiesReadout {

    /** Immutable panel state; center == null means "no center to show". */
    data class State(
        val label: String,
        val verts: Int,
        val edges: Int,
        val faces: Int,
        val center: FloatArray?,
    )

    fun compute(
        activeIndex: Int,
        objectCount: Int,
        engineName: String?,
        engineInfo: IntArray?,
        engineCenter: FloatArray?,
    ): State {
        if (activeIndex < 0 || activeIndex >= objectCount) {
            return State(label = "", verts = 0, edges = 0, faces = 0, center = null)
        }
        val label = if (engineName.isNullOrEmpty()) {
            "Objeto ${activeIndex + 1}"
        } else {
            "$engineName ${activeIndex + 1}"
        }
        return State(
            label = label,
            verts = engineInfo?.getOrNull(0) ?: 0,
            edges = engineInfo?.getOrNull(1) ?: 0,
            faces = engineInfo?.getOrNull(2) ?: 0,
            center = engineCenter,
        )
    }
}
