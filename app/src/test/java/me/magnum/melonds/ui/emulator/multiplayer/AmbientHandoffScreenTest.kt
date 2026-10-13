package me.magnum.melonds.ui.emulator.multiplayer

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class AmbientHandoffScreenTest {
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
