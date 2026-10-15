package me.magnum.melonds.ui.emulator.multiplayer

import org.junit.Assert.assertFalse
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class AmbientHandoffScreenTest {
    @Test
    fun groupsOfTwoOrMoreOfferNetplayHostedOnlyForTwo() {
        val diagnostic = AmbientSlowdownDiagnostic.parse("42\t11.5\t23\t3.0\t48\tIRBO")!!
        assertFalse(AmbientHandoffScreen.SlowLan(diagnostic, 1).canOffer)
        assertTrue(AmbientHandoffScreen.SlowLan(diagnostic, 2).canOffer)
        assertTrue(AmbientHandoffScreen.SlowLan(diagnostic, 4).canOffer)
        assertTrue(AmbientHandoffScreen.SlowLan(diagnostic, 2).canOfferHosted)
        assertFalse(AmbientHandoffScreen.SlowLan(diagnostic, 3).canOfferHosted)
    }

    @Test
    fun slowdownPayloadKeepsNativeLinkMeasurements() {
        assertEquals(
            AmbientSlowdownDiagnostic(42, 11.5, 23, 3.0, 48, "IRBO"),
            AmbientSlowdownDiagnostic.parse("42\t11.5\t23\t3.0\t48\tIRBO"),
        )
        assertNull(AmbientSlowdownDiagnostic.parse("42\tNaN\t23\t3.0\t48\tIRBO"))
        assertNull(AmbientSlowdownDiagnostic.parse("42\t11.5\t23"))
    }

    @Test
    fun onlyPreCommitProgressCanRequestRollback() {
        val cancellable = listOf(
            AmbientHandoffPhase.OFFERED,
            AmbientHandoffPhase.WAITING_PEERS,
            AmbientHandoffPhase.FENCING,
            AmbientHandoffPhase.TRANSFERRING,
            AmbientHandoffPhase.VERIFYING,
        )
        for (phase in cancellable) {
            assertTrue(phase.name, progress(phase).canCancel)
        }
        assertFalse(progress(AmbientHandoffPhase.COMMITTING).canCancel)
        assertFalse(progress(AmbientHandoffPhase.ROLLING_BACK).canCancel)
    }

    private fun progress(phase: AmbientHandoffPhase) = AmbientHandoffScreen.Progress(
        epoch = 12L,
        mode = AmbientHandoffMode.NETPLAY,
        phase = phase,
        peersAccepted = 1,
        peersRequired = 2,
    )
}
