package com.blone.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.blone.app.vm.BloneViewModel
import com.blone.app.vm.SelectionMode
import com.blone.app.vm.Tool
import com.blone.app.vm.ViewportMode

// Dark, focused chrome colors — tuned to sit on top of the GL viewport.
private val ChromeBg = Color(0xCC101623)
private val ChromeFg = Color(0xFFE8EDF5)
private val ChromeDim = Color(0xFF8A94A6)
private val Accent = Color(0xFF2BD4C4)
private val AccentDark = Color(0xFF173B4A)
private val Amber = Color(0xFFFFB84D)
private val DisabledBg = Color(0x661C2534)
private val Danger = Color(0xFF8C2F39)

@Composable
private fun toolButtonColors(selected: Boolean) = ButtonDefaults.buttonColors(
    containerColor = if (selected) AccentDark else DisabledBg,
    contentColor = ChromeFg,
    disabledContainerColor = DisabledBg,
    disabledContentColor = ChromeDim,
)

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
        colors = if (selected) toolButtonColors(true) else ButtonDefaults.buttonColors(
            containerColor = container,
            contentColor = ChromeFg,
            disabledContainerColor = DisabledBg,
            disabledContentColor = ChromeDim,
        ),
        modifier = modifier
            .height(48.dp)
            .padding(horizontal = 2.dp),
    ) {
        Text(text = label, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, maxLines = 1)
    }
}

/**
 * Compose chrome over the dominant viewport:
 *  - top: title, FPS, Desfazer/Refazer (sempre visíveis), Guardar/Abrir;
 *  - bottom: status, barra de modos, e em Editar: seleção V/A/F, ferramentas
 *    explícitas (Mover/Rodar/Escalar + Concluir) e ações de modelação;
 *  - Avançado: primitivas, lista de objetos, nova cena, apagar objeto.
 */
@Composable
fun BloneOverlay(
    viewModel: BloneViewModel,
    onSaveRequest: () -> Unit,
    onLoadRequest: () -> Unit,
) {
    MaterialTheme(
        colorScheme = MaterialTheme.colorScheme.copy(
            primary = Accent,
            onPrimary = Color(0xFF06232B),
            surface = ChromeBg,
            onSurface = ChromeFg,
        )
    ) {
        Box(modifier = Modifier.fillMaxSize()) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .systemBarsPadding()
                    .padding(10.dp),
                verticalArrangement = Arrangement.SpaceBetween
            ) {
                TopBar(viewModel, onSaveRequest, onLoadRequest)
                if (viewModel.advancedOpen) {
                    AdvancedPanel(viewModel)
                }
                BottomBar(viewModel)
            }
        }
    }
}

@Composable
private fun TopBar(viewModel: BloneViewModel, onSave: () -> Unit, onLoad: () -> Unit) {
    Column {
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
                    .background(ChromeBg, RoundedCornerShape(8.dp))
                    .padding(horizontal = 8.dp, vertical = 4.dp)
            )
        }
        Spacer(modifier = Modifier.height(6.dp))
        Row(modifier = Modifier.fillMaxWidth()) {
            ToolButton("↶ Desfazer", false, viewModel.canUndo && !viewModel.busy) { viewModel.undo() }
            ToolButton("↷ Refazer", false, viewModel.canRedo && !viewModel.busy) { viewModel.redo() }
            ToolButton("Guardar", false, !viewModel.busy) { onSave() }
            ToolButton("Abrir", false, !viewModel.busy) { onLoad() }
            Spacer(modifier = Modifier.weight(1f))
            ToolButton("Avançado", viewModel.advancedOpen, !viewModel.busy) { viewModel.toggleAdvanced() }
        }
    }
}

@Composable
private fun AdvancedPanel(viewModel: BloneViewModel) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState())
            .background(ChromeBg, RoundedCornerShape(14.dp))
            .padding(8.dp)
    ) {
        Text("Primitivas", color = ChromeDim, fontSize = 11.sp)
        Row(modifier = Modifier.fillMaxWidth()) {
            ToolButton("Cubo", false) { viewModel.createPrimitive(0, "Cubo") }
            ToolButton("Esfera", false) { viewModel.createPrimitive(1, "Esfera") }
            ToolButton("Cilindro", false) { viewModel.createPrimitive(2, "Cilindro") }
            ToolButton("Plano", false) { viewModel.createPrimitive(3, "Plano") }
        }
        Text("Objetos (${viewModel.objectCount})", color = ChromeDim, fontSize = 11.sp)
        Row(modifier = Modifier.fillMaxWidth()) {
            for (i in 0 until viewModel.objectCount.coerceAtMost(4)) {
                ToolButton("${i + 1}", i == viewModel.activeObject) { viewModel.activateObject(i) }
            }
            if (viewModel.objectCount > 4) {
                Text(
                    "+${viewModel.objectCount - 4}",
                    color = ChromeDim,
                    fontSize = 12.sp,
                    modifier = Modifier.padding(8.dp)
                )
            }
            Spacer(modifier = Modifier.weight(1f))
            ToolButton(
                "Apagar objeto", false,
                viewModel.activeObject >= 0 && !viewModel.busy,
                container = Danger,
            ) { viewModel.deleteActiveObject() }
        }
        Row(modifier = Modifier.fillMaxWidth()) {
            ToolButton("Nova cena", false, !viewModel.busy, container = Danger) {
                viewModel.newScene()
            }
        }
    }
}

@Composable
private fun BottomBar(viewModel: BloneViewModel) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        if (viewModel.armedTool != Tool.NONE) {
            Text(
                text = "${viewModel.armedTool.label} ATIVO — 1 dedo transforma · 2 dedos = câmara",
                color = Amber,
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
                textAlign = TextAlign.Center,
                modifier = Modifier
                    .fillMaxWidth()
                    .background(AccentDark, RoundedCornerShape(8.dp))
                    .padding(horizontal = 12.dp, vertical = 6.dp)
            )
            Spacer(modifier = Modifier.height(6.dp))
        }
        Text(
            text = viewModel.statusText,
            color = ChromeDim,
            fontSize = 12.sp,
            textAlign = TextAlign.Center,
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

        if (viewModel.mode == ViewportMode.EDIT) {
            Spacer(modifier = Modifier.height(6.dp))
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(ChromeBg, RoundedCornerShape(14.dp))
                    .padding(6.dp)
            ) {
                SelectionMode.entries.forEach { sm ->
                    ToolButton(sm.label, viewModel.selectionMode == sm) {
                        viewModel.selectSelectionMode(sm)
                    }
                }
                ToolButton(
                    if (viewModel.multiSelect) "+ múltipla" else "+ única",
                    viewModel.multiSelect,
                ) { viewModel.toggleMultiSelect() }
                ToolButton("Limpar", false, !viewModel.busy) { viewModel.clearSelection() }
            }
            Spacer(modifier = Modifier.height(6.dp))
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(ChromeBg, RoundedCornerShape(14.dp))
                    .padding(6.dp)
            ) {
                ToolButton(Tool.MOVE.label, viewModel.armedTool == Tool.MOVE, !viewModel.busy) {
                    viewModel.armTool(Tool.MOVE)
                }
                ToolButton(Tool.ROTATE.label, viewModel.armedTool == Tool.ROTATE, !viewModel.busy) {
                    viewModel.armTool(Tool.ROTATE)
                }
                ToolButton(Tool.SCALE.label, viewModel.armedTool == Tool.SCALE, !viewModel.busy) {
                    viewModel.armTool(Tool.SCALE)
                }
                ToolButton(
                    "Concluir", false,
                    viewModel.armedTool != Tool.NONE,
                    container = AccentDark,
                ) { viewModel.concludeTool() }
            }
            Spacer(modifier = Modifier.height(6.dp))
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
            }
        }
    }
}
