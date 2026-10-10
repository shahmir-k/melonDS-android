package me.magnum.melonds.ui.emulator.multiplayer

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DirectLinkNameTest {
    @Test
    fun ourNames() {
        assertTrue(DirectLink.isOurName("DIRECT-K7"))
        assertTrue(DirectLink.isOurName("DS_R4M"))
        assertFalse(DirectLink.isOurName("DIRECT-ab-HP OfficeJet"))
        assertFalse(DirectLink.isOurName("DS_R4"))
    }

    @Test
    fun typedNames() {
        assertEquals("DIRECT-K7", DirectLink.normalizeTyped(" direct-k7 "))
        assertEquals("DS_R4M", DirectLink.normalizeTyped("ds_r4m"))
        // Android-picked hotspot names are case-sensitive: kept as typed
        assertEquals("AndroidShare_1234", DirectLink.normalizeTyped("AndroidShare_1234"))
    }
}
