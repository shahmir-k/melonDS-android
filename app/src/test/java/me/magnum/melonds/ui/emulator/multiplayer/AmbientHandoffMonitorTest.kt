package me.magnum.melonds.ui.emulator.multiplayer

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class AmbientHandoffMonitorTest {
    private val slowStats = "links=1 opened=1 slow_offer=1 slow_fps_x100=4210 slow_wait_us=23000 " +
        "slow_exchanges_x100=300 rtt_ms=18"

    @Test fun nativeSlowdownOffersWithoutPausingOrChangingBackend() {
        val monitor = AmbientHandoffMonitor()
        val screen = monitor.screen(snapshot(stats = slowStats), null) as AmbientHandoffScreen.SlowLan
        assertEquals(2, screen.activePlayers)
        assertEquals(42, screen.diagnostic.fps)
        assertEquals(23.0, screen.diagnostic.waitMs, 0.0)
        assertEquals(43, screen.diagnostic.ceilingFps) // wait-only upper bound
        assertTrue(screen.canOffer)
    }

    @Test fun dismissedSlowdownDoesNotResurfaceUntilLinkEnds() {
        val monitor = AmbientHandoffMonitor()
        monitor.dismissSlowForLink()
        assertNull(monitor.screen(snapshot(stats = slowStats), null))
        assertNull(monitor.screen(snapshot(stats = "links=0 slow_offer=0"), null))
        assertTrue(monitor.screen(snapshot(stats = slowStats), null) is AmbientHandoffScreen.SlowLan)
    }

    @Test fun closedOfferDoesNotLoopButNewLinkCanPromptAgain() {
        val monitor = AmbientHandoffMonitor()
        val closed = "closed\t12\t1\t255"
        assertNull(monitor.screen(snapshot(status = closed, stats = slowStats), null))
        assertNull(monitor.screen(snapshot(status = closed, stats = slowStats), null))
        assertTrue(monitor.screen(snapshot(status = closed, stats = slowStats.replace("opened=1", "opened=2")), null)
            is AmbientHandoffScreen.SlowLan)
    }

    @Test fun incomingOfferUsesPeerIdentityAndNeverAutoAccepts() {
        val monitor = AmbientHandoffMonitor()
        val incoming = snapshot(status = "incoming\t12\t2\t255", nearby = listOf("00000000000000ff\tRG DS\tIRBO"))
        val prompt = monitor.screen(incoming, null) as AmbientHandoffScreen.IncomingOffer
        assertEquals("RG DS", prompt.peerName)
        assertEquals(AmbientHandoffMode.HOSTED_NETPLAY, prompt.mode)
        monitor.answered(12, false)
        assertNull(monitor.screen(incoming, prompt))
    }

    @Test fun nativePhasesAndMissingPeerRomAreNotReportedAsSuccess() {
        val monitor = AmbientHandoffMonitor()
        val missing = monitor.screen(snapshot(status = "missing_rom\t12\t1\t255"), null)
        assertTrue(missing is AmbientHandoffScreen.MissingRom)
        val commit = monitor.screen(snapshot(status = "committing\t12\t1\t255"), missing)
            as AmbientHandoffScreen.Progress
        assertEquals(AmbientHandoffPhase.COMMITTING, commit.phase)
        assertNull(monitor.screen(snapshot(status = "committed\t12\t1\t255", netplay = true), commit))
        assertTrue(monitor.screen(snapshot(status = "unresolved\t12\t1\t255"), commit)
            is AmbientHandoffScreen.Unresolved)
    }

    @Test fun malformedNativeStateIsVisibleInsteadOfSilentlyDropped() {
        val monitor = AmbientHandoffMonitor()
        assertTrue(monitor.screen(snapshot(status = "fencing\tnot-an-epoch\t1\t255"), null)
            is AmbientHandoffScreen.Unresolved)
        val failed = "failed\t12\t1\t255"
        assertTrue(monitor.screen(snapshot(status = failed), null) is AmbientHandoffScreen.Failed)
        monitor.dismissTerminal(failed)
        assertNull(monitor.screen(snapshot(status = failed), null))
    }

    @Test fun failureReasonFromNativeIsShown() {
        val monitor = AmbientHandoffMonitor()
        val reason = "the guest rejected our checkpoint: peer JIT settings differ"
        val screen = monitor.screen(snapshot(status = "failed\t12\t1\t255\t$reason"), null)
            as AmbientHandoffScreen.Failed
        assertEquals(reason, screen.reason)
    }

    private fun snapshot(
        status: String = "none",
        stats: String = "",
        nearby: List<String> = emptyList(),
        netplay: Boolean = false,
    ) = AmbientHandoffSnapshot(status, stats, nearby, netplay)
}
