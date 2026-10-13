package me.magnum.melonds.common

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RomStorageAccessTest {
    @Test
    fun selectedFolderContainsExactRom() {
        assertTrue(RomStorageAccess.treeCoversDocument(
            "0123-4567:DS", "0123-4567:DS/Shrek - Smash n' Crash Racing (USA).nds"
        ))
    }

    @Test
    fun selectedAncestorAlsoContainsRom() {
        assertTrue(RomStorageAccess.treeCoversDocument(
            "0123-4567:DS", "0123-4567:DS/Racing/Shrek.nds"
        ))
    }

    @Test
    fun ancestorGrantIsNotTheOriginalRomTree() {
        assertFalse(RomStorageAccess.grantMatchesRomTree(
            "0123-4567:", "0123-4567:DS", "0123-4567:DS/Shrek.nds"
        ))
        assertTrue(RomStorageAccess.grantMatchesRomTree(
            "0123-4567:DS", "0123-4567:DS", "0123-4567:DS/Shrek.nds"
        ))
    }

    @Test
    fun similarlyNamedSiblingDoesNotContainRom() {
        assertFalse(RomStorageAccess.treeCoversDocument(
            "0123-4567:DS", "0123-4567:DS_Backup/Shrek.nds"
        ))
    }

    @Test
    fun anotherVolumeDoesNotContainRom() {
        assertFalse(RomStorageAccess.treeCoversDocument(
            "primary:DS", "0123-4567:DS/Shrek.nds"
        ))
    }

    @Test
    fun absentIdsNeverMatch() {
        assertFalse(RomStorageAccess.treeCoversDocument(null, "0123-4567:DS/Shrek.nds"))
        assertFalse(RomStorageAccess.treeCoversDocument("0123-4567:DS", null))
    }
}
