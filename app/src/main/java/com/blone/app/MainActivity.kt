package com.blone.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.viewmodel.compose.viewModel
import com.blone.app.gl.BloneSurfaceView
import com.blone.app.gl.NativeLib
import com.blone.app.ui.BloneOverlay
import com.blone.app.vm.BloneViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Single-activity, all-Compose host for the Blone viewport.
 *
 * The GL surface is a real OpenGL ES 3.0 context driven by the C++ core;
 * Compose draws the chrome. Project save/open uses the Storage Access
 * Framework — no broad storage permissions, fully offline.
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
    val scope = rememberCoroutineScope()
    val resolver = LocalContext.current.contentResolver

    // SAF: create a .bloneproj file (save) / open an existing one (open).
    val saveLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/octet-stream")
    ) { uri ->
        if (uri != null) {
            scope.launch(Dispatchers.IO) {
                val bytes = NativeLib.saveProject()
                if (bytes == null) {
                    withContext(Dispatchers.Main) { viewModel.saveProjectFailed() }
                } else {
                    val written = try {
                        resolver.openOutputStream(uri)?.use { it.write(bytes) }
                        true
                    } catch (_: Exception) {
                        false
                    }
                    withContext(Dispatchers.Main) {
                        if (written) viewModel.saveProjectOk(bytes) else viewModel.saveProjectFailed()
                    }
                }
            }
        }
    }
    val openLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) {
            scope.launch(Dispatchers.IO) {
                val bytes = try {
                    resolver.openInputStream(uri)?.use { it.readBytes() }
                } catch (_: Exception) {
                    null
                }
                val code = if (bytes != null) NativeLib.openProject(bytes) else -3
                withContext(Dispatchers.Main) { viewModel.openProjectResult(code) }
            }
        }
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xFF0E1218))
    ) {
        AndroidView(
            factory = { ctx ->
                BloneSurfaceView(ctx, viewModel).also { glView.value = it }
            },
            modifier = Modifier.fillMaxSize(),
            update = { view ->
                // Camera pose survives rotation via the ViewModel.
                view.requestRender()
            }
        )

        BloneOverlay(
            viewModel,
            onSaveRequest = {
                try {
                    saveLauncher.launch("blone-projeto.bloneproj")
                } catch (_: Exception) {
                    viewModel.saveProjectFailed()
                }
            },
            onLoadRequest = {
                try {
                    openLauncher.launch(arrayOf("*/*"))
                } catch (_: Exception) {
                    viewModel.openProjectResult(-3)
                }
            },
        )
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
