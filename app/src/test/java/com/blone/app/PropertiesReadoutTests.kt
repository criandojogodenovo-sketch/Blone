package com.blone.app

import com.blone.app.vm.PropertiesReadout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Hotfix D3 (1 BUG = 1 TEST): the properties panel MUST read the ENGINE's
 * active object — including right after an import drain. The Kotlin mirror
 * used to keep a stale `activeObject = -1`, so "Cubo 1" active showed
 * V0·A0·F0 and center "—" and the fixed steps had nothing to act on.
 *
 * The mapping is exercised exactly as [com.blone.app.vm.BloneViewModel]
 * calls it: snapshot (with the engine active index at position 8) decides
 * the active index; the engine name/info/center answers become the readout.
 */
class PropertiesReadoutTests {

    // The engine snapshot layout pinned by NativeLib.selectionSnapshot()
    // (hotfix D3): index 8 = engine active index (-1 when none).
    private fun snapshot(active: Int, hasObject: Boolean = active >= 0): IntArray =
        intArrayOf(
            0,            // editMode (OBJECT)
            2,            // selectionMode (FACE)
            0,            // multiSelect
            if (hasObject) 1 else 0,
            0, 0, 0,      // component selection counts (empty in object mode)
            0,            // hasValidSelection
            active,       // <- the engine truth the mirror must adopt
        )

    @Test
    fun `propriedades mostram o objeto ativo real (nome, contagens, centro)`() {
        val active = snapshot(active = 0)[8]
        val engineName = "Cubo"
        val engineInfo = intArrayOf(8, 12, 6, 1) // [V, E, F, isActive]
        val engineCenter = floatArrayOf(0.25f, 1.0f, -0.5f)

        val ro = PropertiesReadout.compute(
            active, objectCount = 1,
            engineName = engineName,
            engineInfo = engineInfo,
            engineCenter = engineCenter,
        )

        assertEquals("Cubo 1", ro.label)
        assertEquals(8, ro.verts)
        assertEquals(12, ro.edges)
        assertEquals(6, ro.faces)
        assertEquals(3, ro.center?.size)
        assertEquals(0.25f, ro.center?.get(0)!!, 1e-6f)
        assertEquals(1.0f, ro.center?.get(1)!!, 1e-6f)
        assertEquals(-0.5f, ro.center?.get(2)!!, 1e-6f)
    }

    @Test
    fun `espelho antigo vazio nao esconde o objeto ativo (regressao D3)`() {
        // BEFORE the fix: the mirror kept activeObject = -1 while the engine
        // had object 0 active. The snapshot now carries the engine truth.
        val snap = snapshot(active = 0)
        assertEquals(0, snap[8]) // engine says object 0 is active

        val ro = PropertiesReadout.compute(
            snap[8], objectCount = 1,
            engineName = "Cubo",
            engineInfo = intArrayOf(8, 12, 6, 1),
            engineCenter = floatArrayOf(0f, 1f, 0f),
        )

        assertEquals("Cubo 1", ro.label)
        assertEquals(6, ro.faces) // NOT the V0·A0·F0 of the old bug
    }

    @Test
    fun `apos drenagem de importacao o importado e o ativo e aparece`() {
        // The drain (SceneOps::addImportedObject) activates the imported
        // object; the next refresh() snapshot carries index 1 (2nd object).
        val ro = PropertiesReadout.compute(
            activeIndex = 1, objectCount = 2,
            engineName = "cube",
            engineInfo = intArrayOf(24, 36, 12, 1),
            engineCenter = floatArrayOf(2f, 0f, 0f),
        )

        assertEquals("cube 2", ro.label)
        assertEquals(24, ro.verts)
        assertEquals(12, ro.faces)
        assertEquals(2f, ro.center?.get(0)!!, 1e-6f)
    }

    @Test
    fun `sem objeto ativo o painel fica no estado vazio`() {
        val ro = PropertiesReadout.compute(
            activeIndex = -1, objectCount = 0,
            engineName = null, engineInfo = null, engineCenter = null,
        )

        assertEquals("", ro.label)
        assertEquals(0, ro.verts)
        assertEquals(0, ro.edges)
        assertEquals(0, ro.faces)
        assertNull(ro.center)
    }

    @Test
    fun `indice fora do intervalo nao mostra propriedades`() {
        val ro = PropertiesReadout.compute(
            activeIndex = 5, objectCount = 2,
            engineName = "Cubo", engineInfo = intArrayOf(8, 12, 6, 1),
            engineCenter = floatArrayOf(0f, 0f, 0f),
        )

        assertEquals("", ro.label)
        assertNull(ro.center)
    }

    @Test
    fun `nome vazio do motor cai para Objeto N`() {
        val ro = PropertiesReadout.compute(
            activeIndex = 0, objectCount = 1,
            engineName = "",
            engineInfo = intArrayOf(8, 12, 6, 1),
            engineCenter = null,
        )

        assertEquals("Objeto 1", ro.label)
        assertEquals(8, ro.verts)
        assertNull(ro.center)
    }
}
