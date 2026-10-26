package com.blone.app

import com.blone.app.vm.IoStatusParser
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Hotfix D5/D6 — JVM tests of the PURE protocol decoder (IoStatusParser).
 * The ViewModel consumes these events; the dialog mapping depends on them,
 * so the contracts are pinned here without any Android dependency.
 */
class IoStatusParserTests {

    @Test
    fun needsFilesCarriesExactNamesAndMotivo() {
        val ev = IoStatusParser.parse(
            "IMPORT_NEEDS_FILES|cube_ext.bin|o modelo referencia ficheiros " +
                "externos que não foram encontrados junto do documento"
        )
        assertTrue(ev is IoStatusParser.Event.ImportNeedsFiles)
        ev as IoStatusParser.Event.ImportNeedsFiles
        assertEquals(listOf("cube_ext.bin"), ev.files)
        assertEquals(
            "o modelo referencia ficheiros externos que não foram " +
                "encontrados junto do documento",
            ev.motivo,
        )
    }

    @Test
    fun needsFilesSeveralNamesSplitOnSemicolon() {
        val ev = IoStatusParser.parse("IMPORT_NEEDS_FILES|a.bin; b.bin;c.bin|x")
        assertTrue(ev is IoStatusParser.Event.ImportNeedsFiles)
        ev as IoStatusParser.Event.ImportNeedsFiles
        assertEquals(listOf("a.bin", "b.bin", "c.bin"), ev.files)
    }

    @Test
    fun needsFilesWithoutReasonKeepsEmptyMotivo() {
        val ev = IoStatusParser.parse("IMPORT_NEEDS_FILES|solo.bin")
        assertTrue(ev is IoStatusParser.Event.ImportNeedsFiles)
        ev as IoStatusParser.Event.ImportNeedsFiles
        assertEquals(listOf("solo.bin"), ev.files)
        assertEquals("", ev.motivo)
    }

    @Test
    fun importFailFieldsRoundTripIncludingCounts() {
        val ev = IoStatusParser.parse(
            "IMPORT_FAIL|/cache/x.obj|geometria inválida: mal formada|" +
                "1200|600|Médio|75000|150000|1000000"
        )
        assertTrue(ev is IoStatusParser.Event.ImportFail)
        ev as IoStatusParser.Event.ImportFail
        assertEquals("/cache/x.obj", ev.path)
        assertEquals("geometria inválida: mal formada", ev.motivo)
        assertEquals(1200L, ev.fileVerts)
        assertEquals(600L, ev.fileTris)
        assertEquals("Médio", ev.profileName)
        assertEquals(75000L, ev.editVerts)
        assertEquals(150000L, ev.editTris)
        assertEquals(1000000L, ev.viewTris)
    }

    @Test
    fun importFailWithoutCountsDefaultsToUnknownMinusOne() {
        val ev = IoStatusParser.parse("IMPORT_FAIL|/cache/x.obj|ficheiro não encontrado no armazenamento")
        assertTrue(ev is IoStatusParser.Event.ImportFail)
        ev as IoStatusParser.Event.ImportFail
        assertEquals(-1L, ev.fileVerts) // dialog state must NOT open (>= 0 gate)
        assertEquals(-1L, ev.fileTris)
        assertEquals("", ev.profileName)
    }

    @Test
    fun okExportAndPromoteVariantsParse() {
        assertTrue(
            IoStatusParser.parse("IMPORT_OK|Modelo") 
                is IoStatusParser.Event.ImportOk
        )
        val ex = IoStatusParser.parse("EXPORT_OK|/cache/io-export-1.obj")
        assertEquals("/cache/io-export-1.obj", (ex as IoStatusParser.Event.ExportOk).cachePath)
        val exf = IoStatusParser.parse("EXPORT_FAIL|/cache/io-export-1.obj|não foi possível escrever o ficheiro OBJ")
        assertEquals("não foi possível escrever o ficheiro OBJ", (exf as IoStatusParser.Event.ExportFail).motivo)
        val pf = IoStatusParser.parse("PROMOTE_FAIL|Modelo|limite de histórico esgotado para esta promoção")
        assertEquals("Modelo", (pf as IoStatusParser.Event.PromoteFail).name)
        assertTrue(IoStatusParser.parse("KIND_NOVA|x") is IoStatusParser.Event.Unknown)
    }

    // ---- D6: long failure reasons reach the DIALOG state, never cut -------

    /** The VM opens the full-reason dialog for these kinds — ALWAYS. */
    private fun opensFailureDialog(ev: IoStatusParser.Event): Boolean =
        ev is IoStatusParser.Event.ImportFail ||
            ev is IoStatusParser.Event.ExportFail ||
            ev is IoStatusParser.Event.PromoteFail ||
            ev is IoStatusParser.Event.ImportNeedsFiles

    @Test
    fun longImportFailMotivoProducesDialogStateUncut() {
        val reason = "ficheiro acima do limite de edição do perfil Médio " +
            "(150.000 triângulos / 75.000 vértices); o ficheiro tem " +
            "1.250.400 triângulos e 640.120 vértices — " + "x".repeat(2400)
        val status = "IMPORT_FAIL|/cache/io-import-1.gltf|$reason|-1|-1"
        val ev = IoStatusParser.parse(status)
        // D6: the failure produces DIALOG state (not only a statusText line)
        // even with unknown counts, and the motivo arrives COMPLETE.
        assertTrue(opensFailureDialog(ev))
        ev as IoStatusParser.Event.ImportFail
        assertEquals(-1L, ev.fileVerts) // pre-parse failure: no numbers block
        assertEquals(reason, ev.motivo) // NOTHING truncated
        assertEquals(reason.length, ev.motivo.length)
    }

    @Test
    fun longExportFailMotivoProducesDialogStateUncut() {
        val reason = "erro inesperado durante a exportação: " + "y".repeat(1500)
        val ev = IoStatusParser.parse("EXPORT_FAIL|/cache/io-export-1.obj|$reason")
        assertTrue(opensFailureDialog(ev))
        assertEquals(reason, (ev as IoStatusParser.Event.ExportFail).motivo)
    }

    @Test
    fun needsFilesIsDialogStateWithFullMotivo() {
        val motivo = "o modelo referencia ficheiros externos que não foram " +
            "encontrados junto do documento — " + "z".repeat(900)
        val ev = IoStatusParser.parse("IMPORT_NEEDS_FILES|modelo.bin;extra.bin|$motivo")
        assertTrue(opensFailureDialog(ev)) // D6: NEEDS_FILES opens a dialog too
        ev as IoStatusParser.Event.ImportNeedsFiles
        assertEquals(listOf("modelo.bin", "extra.bin"), ev.files)
        assertEquals(motivo, ev.motivo)
    }
}
