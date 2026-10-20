package com.blone.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
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
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
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
private val ChromeSolid = Color(0xFF101623) // fully opaque (bars/panels)
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

/** Compact button for the SIDE TOOLBAR (zone B): ~48 dp touch target. */
@Composable
private fun SideToolButton(
    label: String,
    selected: Boolean,
    enabled: Boolean = true,
    container: Color = DisabledBg,
    onClick: () -> Unit,
) {
    Button(
        onClick = onClick,
        enabled = enabled,
        shape = RoundedCornerShape(10.dp),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(
            horizontal = 2.dp, vertical = 6.dp
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
        modifier = Modifier
            .width(56.dp)
            .height(46.dp)
            .padding(horizontal = 1.dp),
    ) {
        Text(text = label, fontSize = 10.sp, fontWeight = FontWeight.SemiBold, maxLines = 1)
    }
}

/** One segment of the bottom-bar mode/element toggle (zone D). */
@Composable
private fun Segment(
    label: String,
    selected: Boolean,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    Button(
        onClick = onClick,
        shape = RoundedCornerShape(10.dp),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(
            horizontal = 4.dp, vertical = 8.dp
        ),
        colors = if (selected) ButtonDefaults.buttonColors(
            containerColor = AccentDark,
            contentColor = Accent,
        ) else ButtonDefaults.buttonColors(
            containerColor = DisabledBg,
            contentColor = ChromeFg,
        ),
        modifier = modifier
            .height(44.dp)
            .padding(horizontal = 2.dp),
    ) {
        Text(text = label, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, maxLines = 1)
    }
}

/** Small +/− step button for the properties panel (zone C). */
@Composable
private fun StepButton(label: String, enabled: Boolean, onClick: () -> Unit) {
    Button(
        onClick = onClick,
        enabled = enabled,
        shape = RoundedCornerShape(8.dp),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(0.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = DisabledBg,
            contentColor = ChromeFg,
            disabledContainerColor = DisabledBg,
            disabledContentColor = ChromeDim,
        ),
        modifier = Modifier
            .width(44.dp)
            .height(34.dp),
    ) {
        Text(text = label, fontSize = 11.sp, fontWeight = FontWeight.SemiBold, maxLines = 1)
    }
}

/**
 * Compose chrome in FOUR zones (DECISÃO B) over the fullscreen GL viewport:
 *  A. compact TOP BAR — undo/redo, project file actions, mode chip, FPS and
 *     access to the advanced/properties panels (single scrollable row);
 *  B. SIDE TOOLBAR (left) — the tools, in BOTH modes: Seleção/Mover/Rodar/
 *     Escalar plus the contextual modeling operations — the bottom bar is NO
 *     LONGER the tools' home (acceptance criterion);
 *  C. contextual PROPERTIES panel (right, collapsible) — engine-reported
 *     object readout + honest step transforms + help;
 *  D. compact BOTTOM BAR — Objeto/Edição toggle, V/A/F in Edição, selection
 *     status (engine-reported).
 *
 * The four chrome bounds are reported to the renderer as the USABLE viewport
 * rect (top/bottom/left/right in px), so the model is drawn, centered and
 * picked inside the really visible area in EVERY panel state. The scene never
 * leaves the C++ engine when panels open/close — only insets change.
 *
 * Avançado: primitivas, lista de objetos ("Cubo 1"), nova cena, apagar objeto.
 * Fase 3: Esculpir/Pintar are REAL brush modes (bottom bar, 4 segments) with
 * a contextual brush panel (radius/intensity/mode/color — real parameters);
 * the transform tools are disabled while a brush owns the viewport (no dead
 * active buttons).
 */
@Composable
fun BloneOverlay(
    viewModel: BloneViewModel,
    onSaveRequest: () -> Unit,
    onLoadRequest: () -> Unit,
    onUsableViewportChanged: (topPx: Int, bottomPx: Int, leftPx: Int, rightPx: Int) ->
        Unit = { _, _, _, _ -> },
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
            val rootWidthPx = constraints.maxWidth
            val density = LocalDensity.current
            // Landscape caps protect small screens the same way as portrait.
            val isLandscape = rootWidthPx > rootHeightPx
            val bottomCapDp = with(density) {
                ((rootHeightPx * (if (isLandscape) 0.35f else 0.50f)).toInt()).toDp()
            }

            // Measured chrome bounds (window px) -> usable viewport rect.
            var topEdge by remember { mutableIntStateOf(-1) }
            var bottomEdge by remember { mutableIntStateOf(-1) }
            var leftEdge by remember { mutableIntStateOf(-1) }
            var rightEdge by remember { mutableIntStateOf(-1) }
            LaunchedEffect(topEdge, bottomEdge, leftEdge, rightEdge) {
                onUsableViewportChanged(
                    if (topEdge > 0) topEdge else 0,
                    if (bottomEdge > 0) bottomEdge else 0,
                    if (leftEdge > 0) leftEdge else 0,
                    if (rightEdge > 0) rightEdge else 0,
                )
            }

            Column(modifier = Modifier.fillMaxSize().padding(8.dp)) {

                // ---------------- ZONE A: compact top bar (1 scrollable row)
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(ChromeSolid, RoundedCornerShape(14.dp))
                        .padding(6.dp)
                        .onGloballyPositioned { c ->
                            val edge = (c.positionInWindow().y + c.size.height).toInt()
                            if (edge != topEdge) topEdge = edge
                        },
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Row(
                        modifier = Modifier
                            .weight(1f)
                            .horizontalScroll(rememberScrollState()),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            text = "BLONE",
                            color = ChromeFg,
                            fontWeight = FontWeight.Bold,
                            letterSpacing = 2.sp,
                            fontSize = 15.sp,
                            modifier = Modifier.padding(end = 6.dp),
                        )
                        Text(
                            text = viewModel.mode.label,
                            color = Accent,
                            fontSize = 12.sp,
                            fontWeight = FontWeight.SemiBold,
                            modifier = Modifier
                                .background(AccentDark, RoundedCornerShape(8.dp))
                                .padding(horizontal = 8.dp, vertical = 4.dp),
                        )
                        Text(
                            text = viewModel.fpsText,
                            color = ChromeDim,
                            fontFamily = FontFamily.Monospace,
                            fontSize = 12.sp,
                            modifier = Modifier
                                .padding(start = 6.dp)
                                .background(DisabledBg, RoundedCornerShape(8.dp))
                                .padding(horizontal = 8.dp, vertical = 4.dp),
                        )
                        ToolButton("↶", false, viewModel.canUndo && !viewModel.busy) {
                            viewModel.undo()
                        }
                        ToolButton("↷", false, viewModel.canRedo && !viewModel.busy) {
                            viewModel.redo()
                        }
                        ToolButton("Guardar", false, !viewModel.busy) { onSaveRequest() }
                        ToolButton("Abrir", false, !viewModel.busy) { onLoadRequest() }
                        ToolButton("Avançado", viewModel.advancedOpen, !viewModel.busy) {
                            viewModel.toggleAdvanced()
                        }
                        ToolButton("Propriedades", viewModel.propertiesOpen, !viewModel.busy) {
                            viewModel.toggleProperties()
                        }
                    }
                }

                // -------- Avançado (scene-level panel, capped + scrollable)
                if (viewModel.advancedOpen) {
                    AdvancedPanel(viewModel, maxHeightPx = rootHeightPx)
                }

                // --------- middle band: side toolbar | viewport | properties
                Row(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    // ---------------- ZONE B: side toolbar (collapsible-free,
                    // always the same place — frequent ops stay reachable)
                    SideToolbar(
                        viewModel,
                        modifier = Modifier
                            .fillMaxHeight()
                            .onGloballyPositioned { c ->
                                val edge = (c.positionInWindow().x + c.size.width).toInt()
                                if (edge != leftEdge) leftEdge = edge
                            },
                    )

                    // Viewport free space (the GL surface shows through).
                    Spacer(
                        modifier = Modifier
                            .weight(1f)
                            .fillMaxHeight(),
                    )

                    // --------------- ZONE C: contextual properties panel
                    if (viewModel.propertiesOpen) {
                        PropertiesPanel(
                            viewModel,
                            modifier = Modifier
                                .fillMaxHeight()
                                .onGloballyPositioned { c ->
                                    val edge = (rootWidthPx - c.positionInWindow().x).toInt()
                                    if (edge != rightEdge) rightEdge = edge
                                },
                        )
                    }
                }

                // ---------------- ZONE D: compact bottom bar (≤ cap, scroll)
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(max = bottomCapDp)
                        .verticalScroll(rememberScrollState())
                        .onGloballyPositioned { c ->
                            val edge = (rootHeightPx - c.positionInWindow().y).toInt()
                            if (edge != bottomEdge && edge >= 0) bottomEdge = edge
                        },
                ) {
                    BottomBar(viewModel)
                }
            }
        }
    }
}

@Composable
private fun SideToolbar(viewModel: BloneViewModel, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .width(64.dp)
            .heightIn(max = 560.dp)
            .verticalScroll(rememberScrollState())
            .background(ChromeSolid, RoundedCornerShape(14.dp))
            .padding(4.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        // Transform tools — BOTH selection modes. In the brush modes
        // (Esculpir/Pintar) the gizmo is off by design, so these stay
        // DISABLED (never active-without-effect; Fase 4 rule).
        val brushMode = viewModel.brushModeActive
        Tool.entries.forEach { t ->
            if (t == Tool.NONE) {
                SideToolButton("Sel.", viewModel.gizmoTool == Tool.NONE,
                               enabled = !brushMode && !viewModel.busy) {
                    viewModel.chooseGizmoTool(Tool.NONE)
                }
            } else {
                SideToolButton(
                    t.label,
                    viewModel.gizmoTool == t,
                    enabled = !brushMode && !viewModel.busy,
                ) { viewModel.chooseGizmoTool(t) }
            }
        }
        if (brushMode) {
            // Brush mode hint (NOT a button — no dead affordances).
            Text(
                text = "Pincel ativo",
                color = Accent,
                fontSize = 9.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
            )
        }
        if (viewModel.mode == ViewportMode.OBJECT) {
            // Object-mode contextual actions (DECISÃO C).
            SideToolButton("Duplicar", false, enabled = !viewModel.busy) {
                viewModel.duplicateActiveObject()
            }
            SideToolButton("Apagar", false, enabled = viewModel.activeObject >= 0 && !viewModel.busy,
                           container = Danger) {
                viewModel.deleteActiveObject()
            }
        } else if (viewModel.mode == ViewportMode.EDIT) {
            // Edit-mode contextual modeling operations (engine-backed).
            SideToolButton("Extrudir", false, enabled = !viewModel.busy) { viewModel.extrude() }
            SideToolButton("Subdiv.", false, enabled = !viewModel.busy) { viewModel.subdivide() }
            SideToolButton("Apagar", false, enabled = !viewModel.busy, container = Danger) {
                viewModel.deleteSelection()
            }
            SideToolButton("Loop", false, enabled = !viewModel.busy) { viewModel.loopCut() }
            SideToolButton("Limpar", false, enabled = !viewModel.busy) {
                viewModel.clearSelection()
            }
            SideToolButton("Multi", viewModel.multiSelect) { viewModel.toggleMultiSelect() }
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
private fun AdvancedPanel(viewModel: BloneViewModel, maxHeightPx: Int) {
    // The panel never eats the viewport: hard cap + internal scroll.
    val capDp = with(LocalDensity.current) { ((maxHeightPx * 0.40f).toInt()).toDp() }
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(top = 8.dp)
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

/** Axis step row: "X [−] [+]" — honest fixed steps, engine-validated. */
@Composable
private fun StepRow(
    axisLabel: String,
    enabled: Boolean,
    onMinus: () -> Unit,
    onPlus: () -> Unit,
) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            text = axisLabel,
            color = ChromeDim,
            fontSize = 11.sp,
            modifier = Modifier.width(14.dp),
        )
        StepButton("−", enabled, onMinus)
        Spacer(modifier = Modifier.width(4.dp))
        StepButton("+", enabled, onPlus)
    }
}

@Composable
private fun PropertiesPanel(viewModel: BloneViewModel, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .width(216.dp)
            .heightIn(max = 480.dp)
            .verticalScroll(rememberScrollState())
            .background(ChromeSolid, RoundedCornerShape(14.dp))
            .padding(8.dp),
    ) {
        Text(
            text = viewModel.propLabel.ifEmpty { "Nenhum objeto" },
            color = ChromeFg,
            fontSize = 13.sp,
            fontWeight = FontWeight.Bold,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        Text(
            text = "V ${viewModel.propVerts} · A ${viewModel.propEdges} · F ${viewModel.propFaces}",
            color = ChromeDim,
            fontSize = 11.sp,
        )
        val center = viewModel.propCenter
        Text(
            text = if (center != null && center.size >= 3) {
                "Centro (%.1f, %.1f, %.1f)".format(center[0], center[1], center[2])
            } else {
                "Centro —"
            },
            color = ChromeDim,
            fontSize = 11.sp,
            fontFamily = FontFamily.Monospace,
        )
        Spacer(modifier = Modifier.height(8.dp))

        val canTransform = viewModel.activeObject >= 0 && !viewModel.busy
        Text("Mover (passo 0,25)", color = ChromeDim, fontSize = 11.sp)
        StepRow("X", canTransform, { viewModel.nudgeMove(0, -0.25f) },
                { viewModel.nudgeMove(0, 0.25f) })
        StepRow("Y", canTransform, { viewModel.nudgeMove(1, -0.25f) },
                { viewModel.nudgeMove(1, 0.25f) })
        StepRow("Z", canTransform, { viewModel.nudgeMove(2, -0.25f) },
                { viewModel.nudgeMove(2, 0.25f) })
        Spacer(modifier = Modifier.height(6.dp))
        Text("Rodar (passo 15°)", color = ChromeDim, fontSize = 11.sp)
        StepRow("X", canTransform, { viewModel.nudgeRotate(0, -15f) },
                { viewModel.nudgeRotate(0, 15f) })
        StepRow("Y", canTransform, { viewModel.nudgeRotate(1, -15f) },
                { viewModel.nudgeRotate(1, 15f) })
        StepRow("Z", canTransform, { viewModel.nudgeRotate(2, -15f) },
                { viewModel.nudgeRotate(2, 15f) })
        Spacer(modifier = Modifier.height(6.dp))
        Text("Escalar", color = ChromeDim, fontSize = 11.sp)
        Row {
            StepButton("×1,25", canTransform) { viewModel.nudgeScale(3, 1.25f) }
            Spacer(modifier = Modifier.width(4.dp))
            StepButton("÷1,25", canTransform) { viewModel.nudgeScale(3, 0.8f) }
        }
        Spacer(modifier = Modifier.height(8.dp))
        Text(
            text = "Gizmo: arraste um eixo/anel · fora dele a câmara orbita · 2 dedos = câmara",
            color = ChromeDim,
            fontSize = 10.sp,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

// Bottom chrome (zone D): status -> Objeto/Edição toggle -> V/A/F (Edição)
// -> engine-reported selection summary. No expandable "Mais": the tools live
// in the side toolbar now.
@Composable
private fun BottomBar(viewModel: BloneViewModel) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Text(
            text = viewModel.statusText,
            color = ChromeDim,
            fontSize = 12.sp,
            textAlign = TextAlign.Center,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier
                .fillMaxWidth()
                .background(ChromeBg, RoundedCornerShape(8.dp))
                .padding(horizontal = 12.dp, vertical = 6.dp)
        )
        Spacer(modifier = Modifier.height(6.dp))
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(ChromeBg, RoundedCornerShape(14.dp))
                .padding(6.dp)
        ) {
            // Explicit, predictable, reversible mode toggle (DECISÃO C;
            // Fase 3 adds the two REAL brush modes as segments).
            Segment(
                "Objeto",
                viewModel.mode == ViewportMode.OBJECT,
                modifier = Modifier.weight(1f),
            ) { viewModel.selectMode(ViewportMode.OBJECT) }
            Segment(
                "Editar",
                viewModel.mode == ViewportMode.EDIT,
                modifier = Modifier.weight(1f),
            ) { viewModel.selectMode(ViewportMode.EDIT) }
            Segment(
                "Esculpir",
                viewModel.mode == ViewportMode.SCULPT,
                modifier = Modifier.weight(1f),
            ) { viewModel.selectMode(ViewportMode.SCULPT) }
            Segment(
                "Pintar",
                viewModel.mode == ViewportMode.PAINT,
                modifier = Modifier.weight(1f),
            ) { viewModel.selectMode(ViewportMode.PAINT) }
        }
        if (viewModel.brushModeActive) {
            Spacer(modifier = Modifier.height(6.dp))
            BrushPanel(viewModel)
        }
        if (viewModel.mode == ViewportMode.EDIT) {
            Spacer(modifier = Modifier.height(6.dp))
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(ChromeBg, RoundedCornerShape(14.dp))
                    .padding(6.dp)
            ) {
                SelectionMode.entries.forEach { sm ->
                    val label =
                        if (sm == SelectionMode.VERTEX) "V" else if (sm == SelectionMode.EDGE) "A" else "F"
                    Segment(
                        label,
                        viewModel.selectionMode == sm,
                        modifier = Modifier.weight(1f),
                    ) { viewModel.selectSelectionMode(sm) }
                }
            }
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
        }
    }
}

// Curated paint palette (Fase 3): honest, fixed swatches — the selector is
// the row itself. 12 colors cover skin/landscape/contrast needs for now;
// fine HSV tuning is a documented future extension.
private val PaintPalette = listOf(
    0xFFF44336.toInt(), // vermelho
    0xFFFF7043.toInt(), // laranja
    0xFFFFD54F.toInt(), // amarelo
    0xFF66BB6A.toInt(), // verde
    0xFF26A69A.toInt(), // teal
    0xFF42A5F5.toInt(), // azul
    0xFF5C6BC0.toInt(), // indigo
    0xFFAB47BC.toInt(), // roxo
    0xFFEC407A.toInt(), // rosa
    0xFF8D6E63.toInt(), // castanho
    0xFFECEFF1.toInt(), // branco
    0xFF263238.toInt(), // quase preto
)

/** One selectable color swatch (44 dp target, ring when selected). */
@Composable
private fun ColorSwatch(argb: Int, selected: Boolean, onPick: () -> Unit) {
    Box(
        modifier = Modifier
            .padding(horizontal = 4.dp)
            .width(40.dp)
            .height(40.dp)
            .background(Color(argb), CircleShape)
            .border(
                width = if (selected) 3.dp else 1.dp,
                color = if (selected) Accent else ChromeDim,
                shape = CircleShape,
            )
            .clickable(onClick = onPick),
    )
}

/** Labeled slider row for the brush panel (real engine parameters). */
@Composable
private fun BrushSliderRow(
    label: String,
    valueText: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    enabled: Boolean,
    onValueChange: (Float) -> Unit,
) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            text = label,
            color = ChromeDim,
            fontSize = 11.sp,
            modifier = Modifier.width(78.dp),
        )
        Slider(
            value = value,
            onValueChange = onValueChange,
            valueRange = range,
            enabled = enabled,
            colors = SliderDefaults.colors(
                thumbColor = Accent,
                activeTrackColor = AccentDark,
                inactiveTrackColor = DisabledBg,
            ),
            modifier = Modifier
                .weight(1f)
                .height(34.dp),
        )
        Text(
            text = valueText,
            color = ChromeFg,
            fontSize = 11.sp,
            fontFamily = FontFamily.Monospace,
            modifier = Modifier.width(52.dp),
            textAlign = TextAlign.End,
        )
    }
}

/**
 * Contextual brush panel (zone D, bottom bar) for the REAL Esculpir/Pintar
 * modes. Every control feeds a real native parameter: radius (brush extent),
 * intensity (displacement/blend scale), mode (raise/dig) and, for paint, the
 * opacity and the selected color. No decorative controls.
 */
@Composable
private fun BrushPanel(viewModel: BloneViewModel) {
    val sculpt = viewModel.mode == ViewportMode.SCULPT
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(ChromeBg, RoundedCornerShape(14.dp))
            .padding(8.dp)
    ) {
        BrushSliderRow(
            label = "Raio",
            valueText = "%.2f".format(viewModel.brushRadius),
            value = viewModel.brushRadius,
            range = 0.05f..3.0f,
            enabled = true,
            onValueChange = { viewModel.brushRadius = it },
        )
        BrushSliderRow(
            label = "Intensidade",
            valueText = "%d%%".format((viewModel.brushStrength * 100).toInt()),
            value = viewModel.brushStrength,
            range = 0.05f..1.0f,
            enabled = true,
            onValueChange = { viewModel.brushStrength = it },
        )
        if (sculpt) {
            // Surface displacement mode: raise / dig (real SculptMode).
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 2.dp)
            ) {
                Segment(
                    "Levantar",
                    !viewModel.sculptDig,
                    modifier = Modifier.weight(1f),
                ) { viewModel.sculptDig = false }
                Spacer(modifier = Modifier.width(6.dp))
                Segment(
                    "Cavar",
                    viewModel.sculptDig,
                    modifier = Modifier.weight(1f),
                ) { viewModel.sculptDig = true }
            }
        } else {
            BrushSliderRow(
                label = "Opacidade",
                valueText = "%d%%".format((viewModel.brushOpacity * 100).toInt()),
                value = viewModel.brushOpacity,
                range = 0.05f..1.0f,
                enabled = true,
                onValueChange = { viewModel.brushOpacity = it },
            )
            Spacer(modifier = Modifier.height(6.dp))
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .horizontalScroll(rememberScrollState())
            ) {
                PaintPalette.forEach { argb ->
                    ColorSwatch(
                        argb = argb,
                        selected = viewModel.brushColorArgb == argb,
                        onPick = { viewModel.brushColorArgb = argb },
                    )
                }
            }
        }
        Text(
            text = if (sculpt) {
                "Arraste sobre a malha para deformar · um gesto = um passo de desfazer"
            } else {
                "Arraste sobre a malha para pintar · as cores são guardadas no projeto"
            },
            color = ChromeDim,
            fontSize = 10.sp,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(top = 6.dp),
        )
    }
}
