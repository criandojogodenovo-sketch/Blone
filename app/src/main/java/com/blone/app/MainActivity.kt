package com.blone.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.viewmodel.compose.viewModel
import com.blone.app.gl.BloneSurfaceView
import com.blone.app.ui.BloneOverlay
import com.blone.app.vm.BloneViewModel

/**
 * Single-activity, all-Compose host for the Blone viewport.
 *
 * The GL surface is a real OpenGL ES 3.0 context driven by the C++ core;
 * Compose only draws the minimal chrome (title, FPS, mode bar) on top.
 */
class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            BloneRoot()
        }
    }
}

@Composable
private fun BloneRoot() {
    val viewModel: BloneViewModel = viewModel()
    val glView = remember { mutableStateOf<BloneSurfaceView?>(null) }
    val lifecycleOwner = LocalLifecycleOwner.current

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xFF0E1218))
    ) {
        androidx.compose.ui.viewinterop.AndroidView(
            factory = { ctx ->
                BloneSurfaceView(ctx, viewModel).also { glView.value = it }
            },
            modifier = Modifier.fillMaxSize(),
            update = { view ->
                // Camera pose survives rotation via the ViewModel; re-send it
                // so the freshly (re)created surface picks it up immediately.
                view.requestRender()
            }
        )

        BloneOverlay(viewModel)
    }

    // GLSurfaceView requires explicit pause/resume alongside the Activity.
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_PAUSE -> glView.value?.onPause()
                Lifecycle.Event.ON_RESUME -> glView.value?.onResume()
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }
}
