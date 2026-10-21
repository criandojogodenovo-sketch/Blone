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

/** Hotfix D1 (a): the REAL file name for the SAF document — the provider's
 *  DISPLAY_NAME, not the internal document id ("msf:1000123"). */
private fun queryDisplayName(
    resolver: android.content.ContentResolver,
    uri: android.net.Uri,
): String? {
    resolver.query(
        uri,
        arrayOf(android.provider.OpenableColumns.DISPLAY_NAME),
        null, null, null
    )?.use { cursor ->
        val idx = cursor.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
        if (idx >= 0 && cursor.moveToFirst()) {
            val name = cursor.getString(idx)
            if (!name.isNullOrBlank()) return name
        }
    }
    return null
}

@Composable
private fun BloneRoot() {
    val viewModel: BloneViewModel = viewModel()
    val glView = remember { mutableStateOf<BloneSurfaceView?>(null) }
    val lifecycleOwner = LocalLifecycleOwner.current
    val scope = rememberCoroutineScope()
    val resolver = LocalContext.current.contentResolver
    val context = LocalContext.current
    val pendingExportUri = remember { mutableStateOf<android.net.Uri?>(null) }

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

    // ---- Fase 4: I/O 3D (OBJ/glTF) — SAF resolve para ficheiro real -------
    // Hotfix D1, etapa (a): o encaminhamento OBJ vs glTF usa o NOME REAL do
    // ficheiro (OpenableColumns.DISPLAY_NAME). O uri.lastPathSegment é um id
    // interno do provider (ex.: "msf:1000123") — não termina em .obj/.glb —
    // e partia o encaminhamento: um GLB chegava ao parser OBJ e falhava no
    // dispositivo. Cada etapa registada em logcat (tag BloneIO).
    val importLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) {
            scope.launch(Dispatchers.IO) {
                val displayName = try {
                    queryDisplayName(uri)
                } catch (_: Exception) {
                    null
                } ?: uri.lastPathSegment ?: ""
                val mime = try {
                    resolver.getType(uri)
                } catch (_: Exception) {
                    null
                }
                val isGltf = displayName.endsWith(".glb", true) ||
                    displayName.endsWith(".gltf", true) ||
                    (mime?.contains("gltf", true) == true)
                val safeName = displayName.substringBeforeLast('.')
                    .ifEmpty { "Importado" }
                android.util.Log.i(
                    "BloneIO",
                    "(a) SAF: nome='$displayName' mime=$mime -> rota=" +
                        if (isGltf) "glTF/GLB" else "OBJ"
                )
                val tmp = java.io.File(
                    context.cacheDir,
                    "io-import-${System.currentTimeMillis()}${if (isGltf) ".glb" else ".obj"}"
                )
                var copiedBytes = 0L
                val copied = try {
                    resolver.openInputStream(uri)?.use { input ->
                        tmp.outputStream().use { out ->
                            copiedBytes = input.copyTo(out)
                        }
                        true
                    } ?: false
                } catch (e: Exception) {
                    android.util.Log.w(
                        "BloneIO", "(a) SAF: cópia falhou: ${e.message}"
                    )
                    false
                }
                val onDisk = if (copied) tmp.length() else -1L
                android.util.Log.i(
                    "BloneIO",
                    "(a) SAF: lidos=$copiedBytes noDisco=$onDisk tmp=${tmp.name}"
                )
                withContext(Dispatchers.Main) {
                    if (copied && onDisk > 0 && onDisk == copiedBytes) {
                        viewModel.requestImport(tmp.absolutePath, isGltf, safeName)
                    } else {
                        viewModel.importCopyFailed()
                    }
                }
            }
        }
    }
    val exportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("model/obj")
    ) { uri ->
        if (uri != null) {
            pendingExportUri.value = uri
            scope.launch(Dispatchers.IO) {
                val tmp = java.io.File(
                    context.cacheDir,
                    "io-export-${System.currentTimeMillis()}.obj"
                )
                withContext(Dispatchers.Main) { viewModel.requestExport(tmp.absolutePath) }
            }
        }
    }
    // I/O hooks: the VM owns polling + messages; MainActivity owns SAF I/O.
    DisposableEffect(viewModel) {
        viewModel.onExportFileReady = { cachePath ->
            val uri = pendingExportUri.value
            if (uri != null) {
                scope.launch(Dispatchers.IO) {
                    val written = try {
                        resolver.openOutputStream(uri)?.use { out ->
                            java.io.File(cachePath).inputStream().use { it.copyTo(out) }
                        } != null
                    } catch (_: Exception) {
                        false
                    }
                    java.io.File(cachePath).delete() // temp consumed either way
                    withContext(Dispatchers.Main) {
                        if (!written) viewModel.exportCopyFailed()
                    }
                }
            }
        }
        viewModel.onIoFileCleanup = { cachePath ->
            scope.launch(Dispatchers.IO) { java.io.File(cachePath).delete() }
        }
        onDispose {
            viewModel.onExportFileReady = null
            viewModel.onIoFileCleanup = null
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
            onImportRequest = {
                try {
                    importLauncher.launch(
                        arrayOf("model/obj", "model/gltf-binary",
                                "application/octet-stream", "*/*")
                    )
                } catch (_: Exception) {
                    viewModel.importCopyFailed()
                }
            },
            onExportRequest = {
                try {
                    exportLauncher.launch("modelo-blone.obj")
                } catch (_: Exception) {
                    viewModel.exportCopyFailed()
                }
            },
            onUsableViewportChanged = { topPx, bottomPx, leftPx, rightPx ->
                // Chrome bounds -> GL usable viewport rect (top/bottom/left/
                // right). The model is drawn, centered, picked and gizmo-mapped
                // inside the really visible area in every panel state. The GL
                // call is dispatched on the GL thread.
                glView.value?.applyViewportRect(topPx, bottomPx, leftPx, rightPx)
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
