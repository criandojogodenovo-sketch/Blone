package com.blone.app.vm

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.blone.app.gl.CameraState
import com.blone.app.gl.NativeLib
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Viewport tool modes. Sculpt and Paint stay visibly unavailable. */
enum class ViewportMode(val label: String) {
    OBJECT("Objeto"),
    EDIT("Editar"),
    SCULPT("Esculpir"),
    PAINT("Pintar"),
}

/** Element selection mode inside Edit mode. */
enum class SelectionMode(val label: String, val native: Int) {
    VERTEX("Vértice", 0),
    EDGE("Aresta", 1),
    FACE("Face", 2),
}

/** Explicit transform tools (condition 4: two fingers stay camera-only). */
enum class Tool(val label: String, val native: Int) {
    NONE("", 0),
    MOVE("Mover", 1),
    ROTATE("Rodar", 2),
    SCALE("Escalar", 3),
}

/** Human-readable mapping of native status codes (PT). */
object StatusMessages {
    fun forCode(code: Int, action: String): String = when (code) {
        NativeLib.Status.OK -> "$action concluído"
        NativeLib.Status.ERR_NO_SELECTION -> "$action: nada selecionado"
        NativeLib.Status.ERR_NO_ACTIVE_OBJECT -> "$action: nenhum objeto ativo"
        NativeLib.Status.ERR_UNSUPPORTED -> "$action: topologia não suportada"
        NativeLib.Status.ERR_NON_MANIFOLD -> "$action: resultado inválido — malha intacta"
        NativeLib.Status.ERR_BUDGET -> "$action: limite de histórico excedido — malha intacta"
        NativeLib.Status.ERR_LIMIT -> "$action: limite de geometria excedido"
        NativeLib.Status.ERR_INTERNAL -> "$action: falha interna — malha restaurada"
        NativeLib.Status.ERR_INVALID_ARG -> "$action: seleção inválida"
        else -> "$action: erro $code"
    }

    fun projectError(code: Int): String = when (code) {
        0 -> "Projeto aberto"
        -1 -> "Ficheiro inválido (cabeçalho)"
        -2 -> "Versão do formato mais recente — atualize a aplicação"
        -3 -> "Ficheiro inválido (tamanho)"
        -4 -> "Ficheiro corrompido (CRC)"
        -5 -> "Ficheiro excede os limites de geometria"
        -6 -> "Ficheiro com topologia inválida"
        -7 -> "Ficheiro truncado"
        -8 -> "Ficheiro com referências inválidas"
        else -> "Falha ao abrir ($code)"
    }
}

/**
 * Survives configuration changes (rotation). Owns the camera state and all
 * editing UI state; native mutations run on Dispatchers.Default (never the
 * GL thread) and post results back to the main thread.
 */
class BloneViewModel : ViewModel() {

    val camera = CameraState()

    /** Real measured FPS text, updated from the render thread. */
    var fpsText by mutableStateOf("— FPS")
        set

    /** Transient status line shown above the mode bar. */
    var statusText by mutableStateOf("1 dedo: orbitar · 2 dedos: mover e zoom · toque: selecionar")
        set

    var mode by mutableStateOf(ViewportMode.OBJECT)
        private set

    var selectionMode by mutableStateOf(SelectionMode.FACE)
        private set

    /** Gizmo tool currently displayed in edit mode (never blocks gestures). */
    var gizmoTool by mutableStateOf(Tool.MOVE)
        private set

    var multiSelect by mutableStateOf(false)
        private set

    var canUndo by mutableStateOf(false)
        private set

    var canRedo by mutableStateOf(false)
        private set

    var objectCount by mutableStateOf(0)
        private set

    var activeObject by mutableStateOf(-1)
        private set

    // ---- selection state (MIRROR of the C++ engine, never a second truth) --
    // Filled exclusively from NativeLib.selectionSnapshot() by refresh().
    // The Kotlin layer does not keep its own selection set — it displays what
    // the engine reports (atomic snapshot: editMode/mode/multi + counts).
    var engineSelectionMode by mutableStateOf(SelectionMode.FACE)
        private set

    var engineMultiSelect by mutableStateOf(false)
        private set

    var selVerts by mutableStateOf(0)
        private set

    var selEdges by mutableStateOf(0)
        private set

    var selFaces by mutableStateOf(0)
        private set

    /** True when the engine reports at least one ALIVE selected element. */
    var hasValidSelection by mutableStateOf(false)
        private set

    var advancedOpen by mutableStateOf(false)
        private set

    /** Expandable secondary tool panel in Edit mode (contextual bar). */
    var toolsOpen by mutableStateOf(false)
        private set

    /** True while a mutating native operation is in flight. */
    var busy by mutableStateOf(false)
        private set

    fun selectMode(m: ViewportMode) {
        when (m) {
            ViewportMode.OBJECT -> {
                mode = m
                NativeLib.setEditMode(false)
                statusText = "Modo Objeto — toque num objeto para o ativar · 1 dedo orbita · 2 dedos câmara"
                refresh()
            }
            ViewportMode.EDIT -> {
                mode = m
                NativeLib.setEditMode(true)
                statusText = "Editar — toque: selecionar · arraste um eixo do gizmo: transformar · fora: câmara"
                refresh()
            }
            else ->
                statusText = "\"${m.label}\" ainda não está disponível nesta versão."
        }
    }

    fun selectSelectionMode(m: SelectionMode) {
        selectionMode = m
        NativeLib.setSelectionMode(m.native)
        statusText = "Seleção: ${m.label}"
        refresh()
    }

    fun toggleMultiSelect() {
        multiSelect = !multiSelect
        NativeLib.setMultiSelect(multiSelect)
        statusText = if (multiSelect) "Seleção múltipla ativada" else "Seleção única"
        refresh()
    }

    fun clearSelection() {
        NativeLib.clearSelection()
        statusText = "Seleção limpa"
        refresh()
    }

    /** Applies one atomic engine snapshot to the mirrored UI state. */
    private fun applySnapshot(snap: IntArray?) {
        if (snap == null || snap.size < 8) return
        engineSelectionMode = SelectionMode.entries.firstOrNull { it.native == snap[1] }
            ?: SelectionMode.FACE
        engineMultiSelect = snap[2] == 1
        selVerts = snap[4]
        selEdges = snap[5]
        selFaces = snap[6]
        hasValidSelection = snap[7] == 1
    }

    /** Called (UI thread) with the result of [NativeLib.pickAndSelect]:
     *  [kind, objIndex, elemId, nV, nE, nF]; negative kind = error. */
    fun onPickResult(res: IntArray?) {
        if (res == null || res.size < 6) return
        val kind = res[0]
        if (kind < 0) {
            statusText = StatusMessages.forCode(-kind, "Seleção")
            return
        }
        when (kind) {
            1 -> statusText = "Vértice selecionado (${res[3]})"
            2 -> statusText = "Aresta selecionada (${res[4]})"
            3 -> statusText = "Face selecionada (${res[5]})"
            4 -> statusText = if (res[1] >= 0) "Objeto ${res[1] + 1} ativo" else "Nada selecionado"
        }
        refresh()
    }

    // ---- gizmo (correção Fase 2) ---------------------------------------------

    /** True between beginGizmoDrag and end/cancel (plain field, not UI state). */
    private var gizmoDragActive = false

    fun chooseGizmoTool(t: Tool) {
        if (t == Tool.NONE) return
        gizmoTool = t
        NativeLib.setGizmoTool(t.native)
        statusText = when (t) {
            Tool.MOVE -> "Gizmo: Mover — arraste um eixo X/Y/Z · fora do gizmo: câmara"
            Tool.ROTATE -> "Gizmo: Rodar — arraste um anel · fora do gizmo: câmara"
            Tool.SCALE -> "Gizmo: Escalar — arraste um eixo ou o centro (uniforme)"
            Tool.NONE -> ""
        }
    }

    /** Touch-down hit a gizmo handle (UI thread; cheap CPU call). */
    fun beginGizmoDrag(handle: Int, xPx: Float, yPx: Float) {
        val cam = camera
        val code = NativeLib.beginGizmoDrag(
            handle, xPx, yPx,
            cam.azimuthDeg, cam.elevationDeg, cam.distance,
            cam.targetX, cam.targetY, cam.targetZ,
            cam.viewportWidth, cam.viewportHeight,
        )
        gizmoDragActive = code == NativeLib.Status.OK
        if (!gizmoDragActive) {
            statusText = StatusMessages.forCode(code, "Gizmo")
        }
    }

    fun onGizmoDragMove(xPx: Float, yPx: Float) {
        if (!gizmoDragActive) return
        val cam = camera
        NativeLib.updateGizmoDrag(
            xPx, yPx,
            cam.azimuthDeg, cam.elevationDeg, cam.distance,
            cam.targetX, cam.targetY, cam.targetZ,
            cam.viewportWidth, cam.viewportHeight,
        )
    }

    fun onGizmoDragEnd() {
        if (!gizmoDragActive) return
        gizmoDragActive = false
        val code = NativeLib.endGizmoDrag()
        statusText = StatusMessages.forCode(code, "Transformação")
        refresh()
    }

    fun onGizmoDragCancel() {
        if (!gizmoDragActive) return
        gizmoDragActive = false
        NativeLib.cancelGizmoDrag()
        statusText = "Transformação cancelada (câmara assumiu)"
        refresh()
    }

    // ---- modeling operations ------------------------------------------------

    private fun runOp(action: String, onOk: (() -> Unit)? = null, op: () -> Int) {
        if (busy) return
        busy = true
        viewModelScope.launch(Dispatchers.Default) {
            val code = op()
            withContext(Dispatchers.Main) {
                busy = false
                statusText = StatusMessages.forCode(code, action)
                if (code == NativeLib.Status.OK) onOk?.invoke()
                refresh()
            }
        }
    }

    fun extrude() = runOp("Extrusão") { NativeLib.extrude() }
    fun subdivide() = runOp("Subdivisão") { NativeLib.subdivide() }
    fun deleteSelection() = runOp("Apagar") { NativeLib.deleteSelection() }
    fun loopCut() = runOp("Loop cut") { NativeLib.loopCut() }
    fun undo() = runOp("Desfazer") { NativeLib.undo() }
    fun redo() = runOp("Refazer") { NativeLib.redo() }
    fun deleteActiveObject() = runOp("Apagar objeto") { NativeLib.deleteActiveObject() }

    fun createPrimitive(type: Int, name: String) =
        runOp("Criar $name") { NativeLib.createPrimitive(type, 24, 12) }

    fun activateObject(index: Int) {
        NativeLib.setActiveObject(index)
        statusText = "Objeto ${index + 1} ativo"
        refresh()
    }

    fun toggleAdvanced() {
        advancedOpen = !advancedOpen
    }

    fun toggleTools() {
        toolsOpen = !toolsOpen
    }

    fun newScene() = runOp("Nova cena", onOk = {
        mode = ViewportMode.OBJECT
        activeObject = -1
        statusText = "Cena nova — adicione uma primitiva no painel Avançado"
    }, op = { NativeLib.newScene() })

    fun saveProjectOk(bytes: ByteArray) {
        statusText = "Projeto guardado (${bytes.size / 1024} KB)"
    }

    fun saveProjectFailed() {
        statusText = "Falha ao gerar o ficheiro do projeto"
    }

    fun openProjectResult(code: Int) {
        statusText = StatusMessages.projectError(code)
        if (code == 0) {
            // The UI always restarts in Object mode after opening a project;
            // the saved edit state stays inside the file.
            mode = ViewportMode.OBJECT
            NativeLib.setEditMode(false)
        }
        refresh()
    }

    /** Refreshes counters and undo/redo availability from the native scene. */
    fun refresh() {
        canUndo = NativeLib.canUndo()
        canRedo = NativeLib.canRedo()
        objectCount = NativeLib.objectCount()
        if (activeObject >= objectCount) activeObject = objectCount - 1
        // ONE atomic native read for every selection-derived UI state — the
        // Kotlin layer never derives selection state on its own.
        applySnapshot(NativeLib.selectionSnapshot())
    }
}
