package com.blone.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInWindow
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.blone.app.gl.NativeLib
import com.blone.app.vm.BloneViewModel
import com.blone.app.vm.SelectionMode
import com.blone.app.vm.Tool
import com.blone.app.vm.ViewportMode

// Dark, focused chrome colors — tuned to sit on top of the GL viewport.
private val ChromeBg = Color(0xCC101623)
private val ChromeSolid = Color(0xFF101623) // fully opaque (top/bottom bars)
private val ChromeFg = Color(0xFFE8EDF5)
private val ChromeDim = Color(0xFF8A94A6)
private val Accent = Color(0xFF2BD4C4)
private val AccentDark = Color(0xFF173B4A)
private val Amber = Color(0xFFFFB84D)
private val DisabledBg = Color(0x661C2534)
private val Danger = Color(0xFF8C2F39)

@Composable
private fun ToolButton(
    label: String,
    selected: Boolean,
    enabled: Boolean = true,
    container: Color = DisabledBg,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    Button(
        onClick = onClick,
        enabled = enabled,
        shape = RoundedCornerShape(10.dp),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(
            horizontal = 6.dp, vertical = 10.dp
        ),
        colors = if (selected) ButtonDefaults.buttonColors(
            containerColor = AccentDark,
            contentColor = Accent,
        ) else ButtonDefaults.buttonColors(
            containerColor = container,
            contentColor = ChromeFg,
            disabledContainerColor = DisabledBg,
            disabledContentColor = ChromeDim,
        ),
        modifier = modifier
            .height(44.dp)
            .padding(horizontal = 2.dp),
    ) {
        Text(text = label, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, maxLines = 1)
    }
}

/**
 * Compose chrome over the dominant viewport (correção de estabilidade):
 *  - topo: barra SÓLIDA com título, FPS, Desfazer/Refazer, Guardar/Abrir e
 *    Avançado (scroll horizontal — nenhum botão cortado em ecrãs estreitos);
 *  - fundo: estado (linha própria, máx. 2 linhas), barra de modos e, no
 *    Editar, UMA barra contextual compacta (V/A/F + gizmo) com scroll
 *    horizontal e um painel secundário EXPANSÍVEL ("Mais") para as
 *    operações de modelação;
 *  - em horizontal (paisagem) a área combinada dos painéis inferiores fica
 *    limitada a 35% da altura útil, com scroll interno;
 *  - Avançado: primitivas, lista de objetos com nomes legíveis do motor
 *    ("Cubo 1", "Cilindro 2"), nova cena e apagar objeto — "Nova cena"
 *    integralmente visível no topo do painel;
 *  - as alturas reais do chrome são reportadas ao renderer (viewport útil):
 *    o modelo é centrado na área realmente visível, nas duas orientações.
 * Nenhuma funcionalidade foi removida — apenas reorganizada por contexto.
 */
@Composable
fun BloneOverlay(
    viewModel: BloneViewModel,
    onSaveRequest: () -> Unit,
    onLoadRequest: () -> Unit,
    onUsableViewportChanged: (topPx: Int, bottomPx: Int) -> Unit = { _, _ -> },
) {
    MaterialTheme(
        colorScheme = MaterialTheme.colorScheme.copy(
            primary = Accent,
            onPrimary = Color(0xFF06232B),
            surface = ChromeBg,
            onSurface = ChromeFg,
        )
    ) {
        BoxWithConstraints(modifier = Modifier.fillMaxSize()) {
            val rootHeightPx = constraints.maxHeight
            val density = LocalDensity.current
            // Landscape: combined bottom panels ≤ 35% of the usable height;
            // portrait: 50% cap protects small screens the same way.
            val isLandscape = constraints.maxWidth > constraints.maxHeight
            val bottomCapDp = with(density) {
                ((rootHeightPx * (if (isLandscape) 0.35f else 0.50f)).toInt()).toDp()
            }
            var lastTop by remember { mutableStateOf(-1) }
            var lastBottom by remember { mutableStateOf(-1) }

            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(10.dp),
                verticalArrangement = Arrangement.SpaceBetween
            ) {
                Column(
                    modifier = Modifier.onGloballyPositioned { c ->
                        val topPx = (c.positionInWindow().y + c.size.height).toInt()
                        if (topPx != lastTop) {
                            lastTop = topPx
                            onUsableViewportChanged(topPx, if (lastBottom >= 0) lastBottom else 0)
                        }
                    }
                ) {
                    TopBar(viewModel, onSaveRequest, onLoadRequest)
                }
                if (viewModel.advancedOpen) {
                    AdvancedPanel(viewModel, maxHeightPx = rootHeightPx, density = density)
                }
                Column(
                    modifier = Modifier
                        .onGloballyPositioned { c ->
                            val bottomPx = (rootHeightPx - c.positionInWindow().y).toInt()
                            if (bottomPx != lastBottom && bottomPx >= 0) {
                                lastBottom = bottomPx
                                onUsableViewportChanged(if (lastTop >= 0) lastTop else 0, bottomPx)
                            }
                        }
                        .heightIn(max = bottomCapDp)
                        .verticalScroll(rememberScrollState())
                ) {
                    BottomBarContent(viewModel)
                }
            }
        }
    }
}

@Composable
private fun TopBar(viewModel: BloneViewModel, onSave: () -> Unit, onLoad: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(ChromeSolid, RoundedCornerShape(14.dp))
            .padding(6.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = "BLONE",
                color = ChromeFg,
                fontWeight = FontWeight.Bold,
                letterSpacing = 3.sp,
                fontSize = 16.sp
            )
            Spacer(modifier = Modifier.weight(1f))
            Text(
                text = viewModel.fpsText,
                color = ChromeDim,
                fontFamily = FontFamily.Monospace,
                fontSize = 12.sp,
                modifier = Modifier
                    .background(DisabledBg, RoundedCornerShape(8.dp))
                    .padding(horizontal = 8.dp, vertical = 4.dp)
            )
        }
        Spacer(modifier = Modifier.height(6.dp))
        // Scrollable so every button stays fully visible on narrow screens.
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState())
        ) {
            ToolButton("↶", false, viewModel.canUndo && !viewModel.busy) { viewModel.undo() }
            ToolButton("↷", false, viewModel.canRedo && !viewModel.busy) { viewModel.redo() }
            Spacer(modifier = Modifier.width(8.dp))
            ToolButton("Guardar", false, !viewModel.busy) { onSave() }
            ToolButton("Abrir", false, !viewModel.busy) { onLoad() }
            ToolButton("Avançado", viewModel.advancedOpen, !viewModel.busy) { viewModel.toggleAdvanced() }
        }
    }
}

/** Real engine-side object label: type + identifier, e.g. "Cubo 1". */
@Composable
private fun rememberObjectLabel(index: Int): String {
    val base = remember(index) { NativeLib.objectName(index) ?: "" }
    return if (base.isEmpty()) "${index + 1}" else "$base ${index + 1}"
}

@Composable
private fun AdvancedPanel(
    viewModel: BloneViewModel,
    maxHeightPx: Int,
    density: androidx.compose.ui.unit.Density,
) {
    // The panel never eats the viewport: hard cap + internal scroll.
    val capDp = with(density) { ((maxHeightPx * 0.40f).toInt()).toDp() }
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(max = capDp)
            .verticalScroll(rememberScrollState())
            .background(ChromeSolid, RoundedCornerShape(14.dp))
            .padding(8.dp)
    ) {
        // Scene-level actions FIRST: always fully visible without scrolling.
        Row(modifier = Modifier.fillMaxWidth()) {
            ToolButton("Nova cena", false, !viewModel.busy, container = Danger) {
                viewModel.newScene()
            }
            Spacer(modifier = Modifier.width(8.dp))
            ToolButton(
                "Apagar objeto", false,
                viewModel.activeObject >= 0 && !viewModel.busy,
                container = Danger,
            ) { viewModel.deleteActiveObject() }
        }
        Spacer(modifier = Modifier.height(6.dp))
        Text("Primitivas", color = ChromeDim, fontSize = 11.sp)
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState())
        ) {
            ToolButton("Cubo", false) { viewModel.createPrimitive(0, "Cubo") }
            ToolButton("Esfera", false) { viewModel.createPrimitive(1, "Esfera") }
            ToolButton("Cilindro", false) { viewModel.createPrimitive(2, "Cilindro") }
            ToolButton("Plano", false) { viewModel.createPrimitive(3, "Plano") }
        }
        Spacer(modifier = Modifier.height(6.dp))
        Text(
            "Objetos (${viewModel.objectCount}) — toque para ativar",
            color = ChromeDim,
            fontSize = 11.sp,
        )
        // Names come from the engine (type + real identifier), grid of rows.
        val indices = List(viewModel.objectCount) { it }
        indices.chunked(3).forEach { rowIndices ->
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState())
            ) {
                rowIndices.forEach { index ->
                    ToolButton(rememberObjectLabel(index), index == viewModel.activeObject) {
                        viewModel.activateObject(index)
                    }
                }
            }
        }
    }
}

// Bottom chrome: status line -> mode bar -> contextual edit bar (+ "Mais").
// Height is capped by the caller (35% landscape / 50% portrait) with
// internal vertical scroll.
@Composable
private fun BottomBarContent(viewModel: BloneViewModel) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Text(
            text = viewModel.statusText,
            color = ChromeDim,
            fontSize = 12.sp,
            textAlign = TextAlign.Center,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier
                .fillMaxWidth()
                .background(ChromeBg, RoundedCornerShape(8.dp))
                .padding(horizontal = 12.dp, vertical = 6.dp)
        )
        Spacer(modifier = Modifier.height(8.dp))

        // Mode bar: Objeto + Editar enabled; Esculpir/Pintar visibly unavailable.
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(ChromeBg, RoundedCornerShape(14.dp))
                .padding(6.dp)
        ) {
            ViewportMode.entries.forEach { mode ->
                val enabled = mode == ViewportMode.OBJECT || mode == ViewportMode.EDIT
                val selected = viewModel.mode == mode
                Button(
                    onClick = { viewModel.selectMode(mode) },
                    enabled = enabled,
                    shape = RoundedCornerShape(10.dp),
                    contentPadding = androidx.compose.foundation.layout.PaddingValues(
                        horizontal = 4.dp, vertical = 12.dp
                    ),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (selected) AccentDark else DisabledBg,
                        contentColor = ChromeFg,
                        disabledContainerColor = DisabledBg,
                        disabledContentColor = ChromeDim,
                    ),
                    modifier = Modifier
                        .weight(1f)
                        .height(48.dp)
                        .padding(horizontal = 2.dp)
                ) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(text = mode.label, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, maxLines = 1)
                        if (!enabled) {
                            Text(text = "em breve", fontSize = 8.sp, color = ChromeDim)
                        }
                    }
                }
            }
        }

        // Contextual bar — ONLY the tools relevant to the active mode.
        if (viewModel.mode == ViewportMode.EDIT) {
            Spacer(modifier = Modifier.height(6.dp))
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(ChromeBg, RoundedCornerShape(14.dp))
                    .padding(6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                // Scrollable primary row: V/A/F + gizmo tools. No button is
                // ever cut — narrow screens scroll instead.
                Row(
                    modifier = Modifier
                        .weight(1f)
                        .horizontalScroll(rememberScrollState())
                ) {
                    SelectionMode.entries.forEach { sm ->
                        ToolButton(
                            if (sm == SelectionMode.VERTEX) "V" else if (sm == SelectionMode.EDGE) "A" else "F",
                            viewModel.selectionMode == sm,
                        ) { viewModel.selectSelectionMode(sm) }
                    }
                    ToolButton("Mover", viewModel.gizmoTool == Tool.MOVE, !viewModel.busy) {
                        viewModel.chooseGizmoTool(Tool.MOVE)
                    }
                    ToolButton("Rodar", viewModel.gizmoTool == Tool.ROTATE, !viewModel.busy) {
                        viewModel.chooseGizmoTool(Tool.ROTATE)
                    }
                    ToolButton("Escalar", viewModel.gizmoTool == Tool.SCALE, !viewModel.busy) {
                        viewModel.chooseGizmoTool(Tool.SCALE)
                    }
                }
                // Fixed (always reachable) expander for the secondary ops.
                ToolButton("Mais ▾", viewModel.toolsOpen, !viewModel.busy) {
                    viewModel.toggleTools()
                }
            }
            // Engine-reported selection summary (single source of truth).
            Text(
                text = if (viewModel.hasValidSelection) {
                    "Seleção: V ${viewModel.selVerts} · A ${viewModel.selEdges} · F ${viewModel.selFaces}"
                } else {
                    "Seleção vazia — toque num elemento para ativar o gizmo"
                },
                color = if (viewModel.hasValidSelection) Amber else ChromeDim,
                fontSize = 10.sp,
                maxLines = 1,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 10.dp, vertical = 2.dp)
            )
            if (viewModel.toolsOpen) {
                Spacer(modifier = Modifier.height(4.dp))
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(ChromeBg, RoundedCornerShape(14.dp))
                        .padding(6.dp)
                ) {
                    ToolButton("Extrudir", false, !viewModel.busy) { viewModel.extrude() }
                    ToolButton("Subdiv.", false, !viewModel.busy) { viewModel.subdivide() }
                    ToolButton("Apagar", false, !viewModel.busy, container = Danger) {
                        viewModel.deleteSelection()
                    }
                    ToolButton("Loop cut", false, !viewModel.busy) { viewModel.loopCut() }
                    ToolButton(
                        if (viewModel.multiSelect) "+ múltipla" else "+ única",
                        viewModel.multiSelect,
                    ) { viewModel.toggleMultiSelect() }
                    ToolButton("Limpar", false, !viewModel.busy) { viewModel.clearSelection() }
                }
                // Help line: its OWN line, limited to two lines.
                Text(
                    text = "Gizmo: arraste um eixo/anel · fora dele a câmara orbita · 2 dedos = câmara",
                    color = ChromeDim,
                    fontSize = 10.sp,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 10.dp, vertical = 2.dp)
                )
            }
        }
    }
}
