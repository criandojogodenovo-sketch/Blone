package com.blone.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.Button
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
import com.blone.app.vm.ViewportMode

// Dark, focused chrome colors — tuned to sit on top of the GL viewport.
private val ChromeBg = Color(0xCC101623)
private val ChromeFg = Color(0xFFE8EDF5)
private val ChromeDim = Color(0xFF8A94A6)
private val Accent = Color(0xFF2BD4C4)
private val AccentDark = Color(0xFF173B4A)
private val DisabledBg = Color(0x661C2534)

/**
 * Minimal Compose chrome over the dominant viewport:
 *  - top: app title + real measured FPS;
 *  - bottom: mode bar (Object enabled; Edit/Sculpt/Paint visibly unavailable)
 *    and a one-line status/hint message.
 */
@Composable
fun BloneOverlay(viewModel: BloneViewModel) {
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
                TopBar(viewModel)
                BottomBar(viewModel)
            }
        }
    }
}

@Composable
private fun TopBar(viewModel: BloneViewModel) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = "BLONE",
            color = ChromeFg,
            fontWeight = FontWeight.Bold,
            letterSpacing = 3.sp,
            fontSize = 18.sp
        )
        Spacer(modifier = Modifier.weight(1f))
        Text(
            text = viewModel.fpsText,
            color = ChromeDim,
            fontFamily = FontFamily.Monospace,
            fontSize = 13.sp,
            modifier = Modifier
                .background(ChromeBg, RoundedCornerShape(8.dp))
                .padding(horizontal = 10.dp, vertical = 6.dp)
        )
    }
}

@Composable
private fun BottomBar(viewModel: BloneViewModel) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
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
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(ChromeBg, RoundedCornerShape(14.dp))
                .padding(8.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            ViewportMode.entries.forEach { mode ->
                val enabled = mode == ViewportMode.OBJECT
                val selected = viewModel.mode == mode
                Button(
                    onClick = { viewModel.selectMode(mode) },
                    enabled = enabled,
                    shape = RoundedCornerShape(10.dp),
                    contentPadding = androidx.compose.foundation.layout.PaddingValues(
                        horizontal = 8.dp, vertical = 14.dp
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
                ) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(text = mode.label, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                        if (!enabled) {
                            Text(text = "em breve", fontSize = 9.sp, color = ChromeDim)
                        }
                    }
                }
            }
        }
    }
}
