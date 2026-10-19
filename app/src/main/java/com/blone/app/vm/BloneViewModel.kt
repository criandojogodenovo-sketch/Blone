package com.blone.app.vm

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import com.blone.app.gl.CameraState

/** Viewport tool modes. Only OBJECT exists in Phase 1; the rest are visibly unavailable. */
enum class ViewportMode(val label: String) {
    OBJECT("Objeto"),
    EDIT("Editar"),
    SCULPT("Esculpir"),
    PAINT("Pintar"),
}

/**
 * Survives configuration changes (rotation) so the camera pose and selection
 * are preserved when the GL surface is recreated.
 */
class BloneViewModel : ViewModel() {

    val camera = CameraState()

    /** Whether the cube is currently selected (tap picking). */
    var selected by mutableStateOf(false)
        private set

    /** Real measured FPS text, updated from the render thread via the surface view. */
    var fpsText by mutableStateOf("— FPS")
        set

    /** Transient status line shown above the mode bar. */
    var statusText by mutableStateOf("1 dedo: orbitar · 2 dedos: mover e zoom · toque: selecionar")
        set

    var mode by mutableStateOf(ViewportMode.OBJECT)
        private set

    fun selectMode(m: ViewportMode) {
        if (m == ViewportMode.OBJECT) {
            mode = m
            statusText = "Modo Objeto"
        } else {
            // Phase 1: keep the button visible but never pretend it works.
            statusText = "\"${m.label}\" ainda não está disponível nesta versão."
        }
    }

    /** Called (on the UI thread) with the result of a tap picking query. */
    fun onPickResult(hit: Boolean) {
        selected = hit
        statusText = if (hit) "Cubo selecionado" else "Nada selecionado"
    }
}
