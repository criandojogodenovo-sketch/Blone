package com.blone.app.vm

/**
 * Pure parser of the native I/O status protocol (hotfix D5/D6) — JVM-testable
 * by design: the ViewModel applies the state changes, this object ONLY
 * decodes the string. No android.* imports.
 *
 * Protocol (hotfix D1 + F4-LIMITS P1 + hotfix D5):
 *   ""                     idle
 *   "IMPORT_OK|<nome>"
 *   "IMPORT_FAIL|<path>|<motivo PT>|<fileVerts>|<fileTris>
 *              |<perfil>|<editVerts>|<editTris>|<viewTris>"
 *                          (counts -1 when the failure happened before the
 *                           file was parsed; missing fields = legacy message)
 *   "IMPORT_NEEDS_FILES|<nome1;nome2>|<motivo PT>"   (hotfix D5 — external
 *                          buffers of a .gltf; ';' joins the missing names)
 *   "EXPORT_OK|<cachepath>"
 *   "EXPORT_FAIL|<path>|<motivo PT>"
 *   "PROMOTE_OK|<nome>"
 *   "PROMOTE_FAIL|<nome>|<motivo PT>"
 *
 * D6 contract pinned by unit tests: the motivo is carried COMPLETE (never
 * truncated) — long strings must produce dialog state, not a cut status line.
 */
object IoStatusParser {

    sealed class Event {
        data class ImportOk(val name: String) : Event()

        data class ImportFail(
            val path: String,
            val motivo: String,
            val fileVerts: Long,
            val fileTris: Long,
            val profileName: String,
            val editVerts: Long,
            val editTris: Long,
            val viewTris: Long,
        ) : Event()

        /** Hotfix D5: the .gltf references external files that are missing. */
        data class ImportNeedsFiles(
            val files: List<String>,
            val motivo: String,
        ) : Event()

        data class ExportOk(val cachePath: String) : Event()
        data class ExportFail(val path: String, val motivo: String) : Event()
        data class PromoteOk(val name: String) : Event()
        data class PromoteFail(val name: String, val motivo: String) : Event()
        data class Unknown(val kind: String) : Event()
    }

    fun parse(status: String): Event {
        val parts = status.split("|", limit = 9)
        return when (parts.getOrElse(0) { "" }) {
            "IMPORT_OK" -> Event.ImportOk(parts.getOrElse(1) { "" })
            "IMPORT_FAIL" -> Event.ImportFail(
                path = parts.getOrElse(1) { "" },
                motivo = parts.getOrElse(2) { "" },
                fileVerts = parts.getOrNull(3)?.toLongOrNull() ?: -1L,
                fileTris = parts.getOrNull(4)?.toLongOrNull() ?: -1L,
                profileName = parts.getOrElse(5) { "" },
                editVerts = parts.getOrNull(6)?.toLongOrNull() ?: 0L,
                editTris = parts.getOrNull(7)?.toLongOrNull() ?: 0L,
                viewTris = parts.getOrNull(8)?.toLongOrNull() ?: 0L,
            )
            "IMPORT_NEEDS_FILES" -> Event.ImportNeedsFiles(
                files = parts.getOrElse(1) { "" }
                    .split(';')
                    .map { it.trim() }
                    .filter { it.isNotEmpty() },
                motivo = parts.getOrElse(2) { "" },
            )
            "EXPORT_OK" -> Event.ExportOk(parts.getOrElse(1) { "" })
            "EXPORT_FAIL" -> Event.ExportFail(
                path = parts.getOrElse(1) { "" },
                motivo = parts.getOrElse(2) { "" },
            )
            "PROMOTE_OK" -> Event.PromoteOk(parts.getOrElse(1) { "" })
            "PROMOTE_FAIL" -> Event.PromoteFail(
                name = parts.getOrElse(1) { "" },
                motivo = parts.getOrElse(2) { "" },
            )
            else -> Event.Unknown(parts.getOrElse(0) { "" })
        }
    }
}
