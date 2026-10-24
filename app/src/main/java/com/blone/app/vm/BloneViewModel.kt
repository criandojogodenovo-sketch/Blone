package com.blone.app.vm

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.blone.app.gl.CameraState
import com.blone.app.gl.NativeLib
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Viewport tool modes. SCULPT and PAINT are the Fase 3 brush modes. */
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

/** Brush tools for the Fase 3 viewport modes (mirrors the native BrushTool). */
enum class BrushToolKind(val label: String, val native: Int) {
    SCULPT("Esculpir", 0),
    PAINT("Pintar", 1),
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

    /** PT thousands separator (1.000.000) — mirrors the native formatCount. */
    fun formatCount(n: Long): String {
        val digits = n.toString()
        val sb = StringBuilder(digits)
        var i = if (digits.length > 3) digits.length - 3 else 0
        while (i > 0) {
            sb.insert(i, ".")
            i = if (i > 3) i - 3 else 0
        }
        return sb.toString()
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

    /**
     * Import-failure details for the multi-line dialog (F4-LIMITS, P1).
     * The old truncated toast hid the numbers; the dialog shows the file
     * counts, the ACTIVE profile limits and the 3 suggestions. Counts are
     * -1 when the failure happened before the file was parsed.
     */
    data class ImportIssue(
        val motivo: String,
        val fileVerts: Long,
        val fileTris: Long,
        val profileName: String,
        val editVerts: Long,
        val editTris: Long,
        val viewTris: Long,
    )

    var importIssue by mutableStateOf<ImportIssue?>(null)
        private set

    fun dismissImportIssue() {
        importIssue = null
    }

    /**
     * Hotfix D5: a .gltf importada refere buffers externos (.bin) que não
     * vieram com ela — o motor devolveu IMPORT_NEEDS_FILES. O diálogo pede a
     * pasta que os contém; a MainActivity copia-os para a MESMA pasta de
     * cache do .gltf e o import é re-disparado (NativeLib.retryLastImport).
     * O ficheiro .gltf em cache TEM de sobreviver: em NEEDS_FILES não há
     * cleanupTemp — ele é limpo no desfecho final (OK, falha ou cancelamento).
     */
    data class NeedsExternalFiles(
        val files: List<String>,
        val motivo: String,
    )

    var needsFiles by mutableStateOf<NeedsExternalFiles?>(null)
        private set

    /** MainActivity hook: opens the system folder picker (OpenDocumentTree). */
    var onPickExternalFolder: (() -> Unit)? = null

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

    /** F4-LIMITS (P3): 0 = the active object is READ-ONLY (snapshot idx 9). */
    var activeObjectEditable by mutableStateOf(true)
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

    /** Contextual properties panel (right side, collapsible). */
    var propertiesOpen by mutableStateOf(false)
        private set

    /** Expandable secondary tool panel in Edit mode (contextual bar). */
    var toolsOpen by mutableStateOf(false)
        private set

    // ---- brush state (Fase 3) ----------------------------------------------
    // These are REAL operation parameters: they are passed to the native
    // stroke on every begin/move, not merely display values.

    /** Brush radius in world units (native clamps to [0.02, 4]). */
    var brushRadius by mutableStateOf(0.5f)

    /** Stroke intensity in [0.05, 1] (native clamps). */
    var brushStrength by mutableStateOf(0.5f)

    /** Paint opacity in [0.05, 1] (native clamps). */
    var brushOpacity by mutableStateOf(0.85f)

    /** Sculpt displacement mode: false = raise, true = dig. */
    var sculptDig by mutableStateOf(false)

    /** Paint color as ARGB (Compose) — the swatch the user picked. */
    var brushColorArgb by mutableStateOf(0xFFF44336.toInt())

    /** True while a brush gesture owns the finger (plain field, not UI state). */
    private var brushStrokeActive = false

    /** True in the viewport modes that use the brush (drives the touch probe). */
    val brushModeActive: Boolean
        get() = mode == ViewportMode.SCULPT || mode == ViewportMode.PAINT

    /** True while a mutating native operation is in flight. */
    var busy by mutableStateOf(false)
        private set

    // ---- properties panel readout (mirror of the engine, like selection) ----
    // Filled by refresh() from NativeLib — never a second source of truth.
    var propLabel by mutableStateOf("")
        private set

    var propVerts by mutableStateOf(0)
        private set

    var propEdges by mutableStateOf(0)
        private set

    var propFaces by mutableStateOf(0)
        private set

    /** World-space centroid of the active object; null when there is none. */
    var propCenter by mutableStateOf<FloatArray?>(null)
        private set

    fun selectMode(m: ViewportMode) {
        when (m) {
            ViewportMode.OBJECT -> {
                mode = m
                NativeLib.setEditMode(false)
                NativeLib.setGizmoTool(gizmoTool.native) // restore the gizmo
                statusText = "Modo Objeto — toque num objeto para o ativar · 1 dedo orbita · 2 dedos câmara"
                refresh()
            }
            ViewportMode.EDIT -> {
                mode = m
                NativeLib.setEditMode(true)
                NativeLib.setGizmoTool(gizmoTool.native) // restore the gizmo
                statusText = "Editar — toque: selecionar · arraste um eixo do gizmo: transformar · fora: câmara"
                refresh()
            }
            ViewportMode.SCULPT -> {
                mode = m
                NativeLib.setEditMode(false)
                NativeLib.setGizmoTool(Tool.NONE.native) // the brush owns the view
                statusText = "Esculpir — arraste sobre a superfície · fora dela: câmara · 2 dedos câmara"
                refresh()
            }
            ViewportMode.PAINT -> {
                mode = m
                NativeLib.setEditMode(false)
                NativeLib.setGizmoTool(Tool.NONE.native) // the brush owns the view
                statusText = "Pintar — arraste sobre a superfície para pintar · fora dela: câmara"
                refresh()
            }
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

    /** Applies one atomic engine snapshot to the mirrored UI state.
     *
     *  Hotfix D3: index 8 is the ENGINE's active object. The Kotlin mirror
     *  used to keep its own stale copy (set only by explicit activation), so
     *  the properties panel showed "Nenhum objeto"/V0·A0·F0 with an active
     *  object and the fixed steps had nothing to act on. From now on the
     *  engine is the single source of truth — after a pick, a primitive,
     *  an import drain, undo/redo, the mirror re-syncs every refresh(). */
    private fun applySnapshot(snap: IntArray?) {
        if (snap == null || snap.size < 8) return
        engineSelectionMode = SelectionMode.entries.firstOrNull { it.native == snap[1] }
            ?: SelectionMode.FACE
        engineMultiSelect = snap[2] == 1
        selVerts = snap[4]
        selEdges = snap[5]
        selFaces = snap[6]
        hasValidSelection = snap[7] == 1
        if (snap.size >= 9) activeObject = snap[8]
        if (snap.size >= 10) activeObjectEditable = snap[9] == 1
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
        gizmoTool = t
        NativeLib.setGizmoTool(t.native)
        statusText = when (t) {
            Tool.MOVE -> "Gizmo: Mover — arraste um eixo X/Y/Z · fora do gizmo: câmara"
            Tool.ROTATE -> "Gizmo: Rodar — arraste um anel · fora do gizmo: câmara"
            Tool.SCALE -> "Gizmo: Escalar — arraste um eixo ou o centro (uniforme)"
            Tool.NONE -> "Seleção — toque para selecionar · gizmo desligado"
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
            // Hotfix D7: a rejeição de Rodar/Escalar num objeto em modo
            // leitura é POLÍTICA (não topologia) — a mensagem diz a verdade
            // (R8) e aponta para o caminho certo.
            statusText = if (!activeObjectEditable) {
                "Objeto em modo leitura: só Mover está disponível — use “Tornar editável” para Rodar/Escalar."
            } else {
                StatusMessages.forCode(code, "Gizmo")
            }
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

    // ---- brush gestures (Fase 3) --------------------------------------------
    // Same shape as the gizmo drag path: the surface view feeds touch events
    // in, the native engine does the work, Kotlin owns only the parameters.

    /**
     * Touch-down in a brush mode: hits the surface and starts a stroke.
     * Returns true when a stroke is active (the gesture becomes a stroke,
     * never an orbit).
     */
    fun beginBrushAt(xPx: Float, yPx: Float): Boolean {
        if (mode != ViewportMode.SCULPT && mode != ViewportMode.PAINT) return false
        val tool = if (mode == ViewportMode.SCULPT) BrushToolKind.SCULPT else BrushToolKind.PAINT
        // Honest probe: without a surface under the finger no stroke starts
        // (the gesture falls through to the camera orbit path).
        val hit = NativeLib.brushHit(xPx, yPx)
        if (hit == null || hit.isEmpty() || hit[0] < 0.5f) return false
        val c = brushColorArgb
        val r = ((c shr 16) and 0xFF) / 255f
        val g = ((c shr 8) and 0xFF) / 255f
        val b = (c and 0xFF) / 255f
        val code = NativeLib.beginBrushStroke(
            tool = tool.native,
            xPx = xPx,
            yPx = yPx,
            radius = brushRadius,
            strength = brushStrength,
            mode = if (sculptDig) 1 else 0,
            r = r,
            g = g,
            b = b,
            opacity = brushOpacity,
        )
        brushStrokeActive = code == NativeLib.Status.OK
        if (!brushStrokeActive && code != 0) {
            statusText = StatusMessages.forCode(code, "Pincel")
        }
        return brushStrokeActive
    }

    fun onBrushMove(xPx: Float, yPx: Float) {
        if (!brushStrokeActive) return
        val code = NativeLib.moveBrushStroke(xPx, yPx, brushRadius, brushStrength)
        if (code != NativeLib.Status.OK && code != 0) {
            statusText = StatusMessages.forCode(code, "Pincel")
        }
    }

    fun onBrushEnd() {
        if (!brushStrokeActive) return
        brushStrokeActive = false
        val code = NativeLib.endBrushStroke()
        statusText = StatusMessages.forCode(code, "Pincel")
        refresh()
    }

    fun onBrushCancel() {
        if (!brushStrokeActive) return
        brushStrokeActive = false
        NativeLib.cancelBrushStroke()
        statusText = "Traço cancelado (câmara assumiu)"
        refresh()
    }

    // ---- I/O 3D (Fase 4: OBJ/glTF) -----------------------------------------
    //
    // The ViewModel owns ONLY the polling and the messages; SAF file I/O
    // stays in MainActivity through the two hooks below (the VM never
    // touches android.net.Uri). The native side is asynchronous: parse/build
    // happen on a worker, the object enters the scene at the GL-thread drain
    // via the SceneOps facade (one undo entry per import).

    /** True while an import/export job is pending (drives the polling). */
    var ioBusy by mutableStateOf(false)
        private set

    /** F4-LIMITS (P4): the kind of the running job (dialog title/labels). */
    enum class IoJobKind { IMPORT, EXPORT, PROMOTE }

    var ioJobKind by mutableStateOf(IoJobKind.IMPORT)
        private set

    /** Import progress 0..1000 from the engine (P4; polled with the status). */
    var ioProgress by mutableStateOf(0)
        private set

    // ---- limits profile (F4-LIMITS, P6) ---------------------------------
    // 0 Baixo, 1 Médio (default until the Activity applies the saved choice
    // or the RAM-appropriate one), 2 Alto. The native side is the single
    // source of truth for the numbers (core/io/GeometryLimits.h).
    var limitsProfile by mutableStateOf(1)
        private set

    /** MainActivity hook: persists the profile choice (SharedPreferences). */
    var onProfilePersist: ((profile: Int) -> Unit)? = null

    /** Called ONCE at startup (MainActivity): saved choice or RAM default. */
    fun initLimitsProfile(profile: Int) {
        val p = profile.coerceIn(0, 2)
        limitsProfile = p
        NativeLib.setLimitsProfile(p)
    }

    fun selectLimitsProfile(profile: Int) {
        if (profile !in 0..2 || profile == limitsProfile) return
        limitsProfile = profile
        NativeLib.setLimitsProfile(profile)
        statusText = when (profile) {
            0 -> "Perfil Baixo — objetos até 80.000 triângulos editáveis · histórico 32 MB"
            2 -> "Perfil Alto — objetos até 300.000 triângulos editáveis · usa mais memória"
            else -> "Perfil Médio — objetos até 150.000 triângulos editáveis"
        }
        onProfilePersist?.invoke(profile)
    }

    /** MainActivity hook: copies the exported cache file to the SAF uri
     *  (Dispatchers.IO) and deletes the temporary. */
    var onExportFileReady: ((cachePath: String) -> Unit)? = null

    /** MainActivity hook: deletes a temporary cache file. */
    var onIoFileCleanup: ((cachePath: String) -> Unit)? = null

    private var ioWatchJob: Job? = null
    private var ioTempPath: String? = null

    /** D5: cache copies of the external buffers — cleaned with the temp. */
    private var externalTempPaths: List<String> = emptyList()

    /** Enqueues an import job over an already-copied cache file. */
    fun requestImport(cachePath: String, isGltf: Boolean, displayName: String) {
        if (ioBusy) {
            statusText = "Já existe uma operação de E/S em curso"
            return
        }
        ioTempPath = cachePath
        ioJobKind = IoJobKind.IMPORT
        ioProgress = 0
        statusText = "A importar…"
        if (NativeLib.importMesh(cachePath, isGltf, displayName)) {
            beginIoWatch()
        } else {
            finishIo("Falha na importação")
        }
    }

    /** Enqueues an OBJ export job over a fresh cache file. */
    fun requestExport(cachePath: String) {
        if (ioBusy) {
            statusText = "Já existe uma operação de E/S em curso"
            return
        }
        ioTempPath = cachePath
        ioJobKind = IoJobKind.EXPORT
        statusText = "A exportar…"
        if (NativeLib.exportObj(cachePath)) {
            beginIoWatch()
        } else {
            finishIo("Falha na exportação")
        }
    }

    /** The SAF copy URI -> cache failed before any job was enqueued
     *  (hotfix D1/R8: a falha diz o motivo — nunca silenciosa). */
    fun importCopyFailed() {
        statusText = "Falha na importação: não foi possível ler o ficheiro"
        cleanupTemp()
    }

    /** The SAF copy cache -> uri failed after a successful export. */
    fun exportCopyFailed() {
        statusText = "Falha na exportação"
    }

    private fun finishIo(message: String) {
        statusText = message
        cleanupTemp()
    }

    private fun cleanupTemp() {
        val path = ioTempPath
        ioTempPath = null
        val extras = externalTempPaths
        externalTempPaths = emptyList()
        if (path != null) onIoFileCleanup?.invoke(path)
        for (extra in extras) onIoFileCleanup?.invoke(extra)
    }

    /** 400 ms polling — runs ONLY while a job is pending (ioBusy). */
    private fun beginIoWatch() {
        ioBusy = true
        ioWatchJob?.cancel()
        ioWatchJob = viewModelScope.launch {
            while (ioBusy) {
                delay(400)
                // P4: keep the progress dialog alive while the job runs.
                if (ioJobKind == IoJobKind.IMPORT) {
                    ioProgress = NativeLib.importProgress()
                }
                val status = NativeLib.ioStatusConsume()
                if (status.isNotEmpty()) handleIoStatus(status)
            }
        }
    }

    /** P4: user pressed Cancelar — the loaders unwind cleanly. */
    fun cancelIoJob() {
        if (ioJobKind == IoJobKind.IMPORT) {
            NativeLib.cancelImport()
            statusText = "A cancelar…"
        }
    }

    private fun handleIoStatus(status: String) {
        // Protocol decoded by the PURE parser (hotfix D5/D6, JVM-tested):
        // the kind + payload + the human-readable reason produced by the
        // native stage that failed; IMPORT_FAIL fields 4..9 carry the
        // machine-readable counts/limits for the failure dialog (-1 =
        // unknown). Missing fields = legacy message (statusText only).
        when (val ev = IoStatusParser.parse(status)) {
            is IoStatusParser.Event.ImportOk -> {
                statusText = "Importado: ${ev.name}"
                ioBusy = false
                cleanupTemp() // the cache copy has served its purpose
                refresh()
            }
            is IoStatusParser.Event.ImportFail -> {
                val motivo = ev.motivo
                statusText = if (motivo.isEmpty()) "Falha na importação"
                             else "Falha na importação: $motivo"
                // Dialog whenever the file counts were reportable (parsed
                // or drained). Open/parser failures keep the full reason in
                // the status line (never a bare message, R8).
                if (ev.fileVerts >= 0) {
                    importIssue = ImportIssue(
                        motivo = motivo.ifEmpty { "falha na importação" },
                        fileVerts = ev.fileVerts,
                        fileTris = ev.fileTris,
                        profileName = ev.profileName.ifEmpty { "Médio" },
                        editVerts = ev.editVerts,
                        editTris = ev.editTris,
                        viewTris = ev.viewTris,
                    )
                }
                ioBusy = false
                cleanupTemp()
                refresh()
            }
            is IoStatusParser.Event.ImportNeedsFiles -> {
                // D5: dois tempos — pedir a pasta que contém os .bin. O
                // .gltf em cache TEM de sobreviver para o retry (sem
                // cleanupTemp aqui); o motivo completo fica no diálogo.
                needsFiles = NeedsExternalFiles(ev.files, ev.motivo)
                statusText = "Ficheiros externos em falta: " +
                    ev.files.joinToString(", ")
                ioBusy = false
            }
            is IoStatusParser.Event.ExportOk -> {
                statusText = "Exportado"
                ioBusy = false
                val path = ioTempPath
                ioTempPath = null
                // Cache -> SAF uri copy (and temp deletion) is the
                // MainActivity hook — the VM does no Android I/O.
                onExportFileReady?.invoke(path ?: ev.cachePath)
            }
            is IoStatusParser.Event.ExportFail -> {
                val motivo = ev.motivo
                statusText = if (motivo.isEmpty()) "Falha na exportação"
                             else "Falha na exportação: $motivo"
                ioBusy = false
                cleanupTemp()
            }
            is IoStatusParser.Event.PromoteOk -> {
                statusText = "Objeto editável: ${ev.name}"
                ioBusy = false
                refresh()
            }
            is IoStatusParser.Event.PromoteFail -> {
                val motivo = ev.motivo
                statusText = if (motivo.isEmpty()) "Falha ao tornar editável"
                             else "Falha ao tornar editável: $motivo"
                ioBusy = false
                refresh()
            }
            is IoStatusParser.Event.Unknown -> Unit
        }
    }

    // ---- D5: two-step import for .gltf with external buffers ---------------

    /** User tapped "Escolher pasta" on the NEEDS_FILES dialog. */
    fun requestNeedsFilesFolder() {
        if (needsFiles == null) return
        onPickExternalFolder?.invoke()
    }

    /**
     * MainActivity hook result: the picked folder was scanned. `ok` = every
     * missing file was found and copied next to the cached .gltf; the
     * import is then re-fired natively (same spec — retryLastImport).
     * `copiedPaths` are cache temporaries cleaned with the flow's outcome.
     */
    fun onExternalFilesCopied(ok: Boolean, copiedPaths: List<String>) {
        if (needsFiles == null) return
        if (!ok) {
            // Honest retry loop: the folder did not contain them — the
            // dialog stays open with a clear PT reason (R8).
            statusText = "A pasta escolhida não contém: " +
                (needsFiles?.files?.joinToString(", ") ?: "") +
                " — escolha outra pasta ou cancele."
            return
        }
        externalTempPaths = copiedPaths
        needsFiles = null
        retryAfterExternalFiles()
    }

    /** User cancelled the NEEDS_FILES dialog: clear failure reason (R8). */
    fun cancelNeedsFiles() {
        val files = needsFiles?.files.orEmpty()
        needsFiles = null
        statusText = if (files.isEmpty()) {
            "Importação cancelada — nada foi importado."
        } else {
            "Importação cancelada: o modelo precisa dos ficheiros externos (" +
                files.joinToString(", ") + ") — nada foi importado."
        }
        cleanupTemp()
    }

    private fun retryAfterExternalFiles() {
        if (ioBusy) return
        ioJobKind = IoJobKind.IMPORT
        ioProgress = 0
        statusText = "A importar…"
        if (NativeLib.retryLastImport()) {
            beginIoWatch()
        } else {
            finishIo("Falha ao reiniciar a importação")
        }
    }

    /**
     * F4-LIMITS (P3): "Tornar editável" for the ACTIVE read-only object.
     * Shares the I/O polling loop (ioBusy) with import/export.
     */
    fun requestPromoteActive() {
        if (ioBusy) {
            statusText = "Já existe uma operação de E/S em curso"
            return
        }
        ioJobKind = IoJobKind.PROMOTE
        statusText = "A tornar editável…"
        if (NativeLib.promoteActiveObject()) {
            beginIoWatch()
        } else {
            statusText = "Nada para tornar editável"
        }
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

    /** Duplicates the ACTIVE object; the copy becomes active (engine side). */
    fun duplicateActiveObject() = runOp("Duplicar objeto") {
        val idx = NativeLib.duplicateActiveObject()
        if (idx >= 0) {
            activeObject = idx // the engine activated the copy
            NativeLib.Status.OK
        } else {
            -idx // positive status code -> readable StatusMessages entry
        }
    }

    // ---- direct step transforms (properties panel) ---------------------------
    // Fixed, honest steps — no free-form numeric entry yet (documented).
    fun nudgeMove(axis: Int, worldUnits: Float) =
        runOp("Mover") { NativeLib.transformDirect(Tool.MOVE.native, axis, worldUnits) }

    fun nudgeRotate(axis: Int, degrees: Float) =
        runOp("Rodar") { NativeLib.transformDirect(Tool.ROTATE.native, axis, degrees) }

    fun nudgeScale(axis: Int, factor: Float) =
        runOp("Escalar") { NativeLib.transformDirect(Tool.SCALE.native, axis, factor) }

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

    fun toggleProperties() {
        propertiesOpen = !propertiesOpen
    }

    fun toggleTools() {
        toolsOpen = !toolsOpen
    }

    fun newScene() = runOp("Nova cena", onOk = {
        mode = ViewportMode.OBJECT
        activeObject = -1
        NativeLib.setGizmoTool(gizmoTool.native)
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
            NativeLib.setGizmoTool(gizmoTool.native)
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
        // Properties readout: engine-reported name/counts/center only, keyed
        // by the ENGINE active index synced above (hotfix D3).
        val inRange = activeObject in 0 until objectCount
        val readout = PropertiesReadout.compute(
            activeObject, objectCount,
            if (inRange) NativeLib.objectName(activeObject) else null,
            if (inRange) NativeLib.objectInfo(activeObject) else null,
            if (inRange) NativeLib.objectCenter(activeObject) else null,
        )
        propLabel = readout.label
        propVerts = readout.verts
        propEdges = readout.edges
        propFaces = readout.faces
        propCenter = readout.center
    }
}
