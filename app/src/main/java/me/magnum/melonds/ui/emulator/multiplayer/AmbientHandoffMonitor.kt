package me.magnum.melonds.ui.emulator.multiplayer

import kotlin.math.min

/** A read-only snapshot of the native Just Play backend. Never advances the emulator. */
internal data class AmbientHandoffSnapshot(
    val status: String,
    val stats: String,
    val nearby: List<String>,
    val netplayActive: Boolean,
)

/**
 * Converts the emulator's status into presentation state. The native coordinator remains the
 * authority for offers, checkpoint ownership, commit and rollback; this class cannot switch a
 * backend or pause a game. In particular, a dialog must not use the restart-based Netplay path.
 */
internal class AmbientHandoffMonitor {
    private var answeredEpoch = 0L
    private var acceptedAnswer = false
    private var dismissedTerminal = ""
    private var slowDismissedForLink = false
    private var lastClosedStatus = ""
    private var lastLinksOpened = -1L

    fun beginOffer() {
        dismissedTerminal = ""
    }

    fun answered(epoch: Long, accepted: Boolean) {
        dismissedTerminal = ""
        answeredEpoch = epoch
        acceptedAnswer = accepted
    }

    fun dismissTerminal(status: String) {
        dismissedTerminal = status
    }

    fun dismissSlowForLink() {
        slowDismissedForLink = true
    }

    fun screen(snapshot: AmbientHandoffSnapshot, current: AmbientHandoffScreen?): AmbientHandoffScreen? {
        if (snapshot.netplayActive) {
            slowDismissedForLink = false
            return null
        }
        val parts = snapshot.status.split('\t')
        val phase = parts[0]
        // an offer's answer lasts only as long as that offer: a later offer that reuses the epoch
        // (another session) must be asked again, not shown as already accepted
        if (phase == "none" || phase == "closed" || phase == "failed" || phase == "committed") answeredEpoch = 0L
        if (phase == "committed") return null
        if (phase == "closed" && snapshot.status != lastClosedStatus) {
            // A declined or cancelled switch should not immediately re-offer
            // the same slow LAN link while the native terminal status persists.
            slowDismissedForLink = true
            lastClosedStatus = snapshot.status
        }
        if (phase == "none" || phase.isEmpty() || phase == "closed" ||
            (phase == "failed" && snapshot.status == dismissedTerminal)) {
            val counters = counters(snapshot.stats)
            val links = counters["links"]?.toIntOrNull() ?: return null
            val opened = counters["opened"]?.toLongOrNull()
            if (opened != null) {
                if (lastLinksOpened >= 0 && opened > lastLinksOpened) slowDismissedForLink = false
                lastLinksOpened = opened
            }
            if (links == 0) {
                slowDismissedForLink = false
                return null
            }
            if (slowDismissedForLink) return null
            if (current is AmbientHandoffScreen.Progress && current.epoch == 0L) return current
            if (current is AmbientHandoffScreen.SlowLan) return current
            if (counters["slow_offer"] != "1") return null
            val fpsX100 = counters["slow_fps_x100"]?.toIntOrNull() ?: return null
            val waitUs = counters["slow_wait_us"]?.toIntOrNull() ?: return null
            val exchangesX100 = counters["slow_exchanges_x100"]?.toIntOrNull() ?: return null
            val rtt = counters["rtt_ms"]?.toIntOrNull() ?: return null
            if (fpsX100 < 0 || waitUs < 0 || exchangesX100 < 0 || rtt < 0) return null
            // A frame cannot finish faster than its measured network wait. This is only a
            // wait-based upper bound, not a forecast of full emulation or rendering speed.
            val waitOnlyCeiling = if (waitUs == 0) 60 else min(60, 1_000_000 / waitUs)
            val diagnostic = AmbientSlowdownDiagnostic(
                fps = (fpsX100 + 50) / 100,
                waitMs = waitUs / 1000.0,
                rttMs = rtt,
                exchangesPerFrame = exchangesX100 / 100.0,
                ceilingFps = waitOnlyCeiling,
                gameCode = "",
            )
            return AmbientHandoffScreen.SlowLan(diagnostic, links + 1)
        }

        if (snapshot.status == dismissedTerminal) return null
        // failed/unresolved carry the native reason as a fifth field
        val reason = parts.getOrNull(4)?.takeIf { it.isNotBlank() }
        if (phase == "failed") return AmbientHandoffScreen.Failed(reason ?: "The native switch failed; LAN was restored.")
        if (phase == "unresolved") return AmbientHandoffScreen.Unresolved(reason ?: "The native commit could not be confirmed.")
        if (parts.size != 4) return AmbientHandoffScreen.Unresolved("Invalid native handoff status.")
        val epoch = parts[1].toLongOrNull() ?: return AmbientHandoffScreen.Unresolved("Invalid handoff epoch.")
        val mode = when (parts[2]) {
            "1" -> AmbientHandoffMode.NETPLAY
            "2" -> AmbientHandoffMode.HOSTED_NETPLAY
            else -> return AmbientHandoffScreen.Unresolved("Invalid handoff mode.")
        }
        val peerId = parts[3].toULongOrNull()
        val peerName = snapshot.nearby.firstNotNullOfOrNull { row ->
            val fields = row.split('\t')
            if (fields.size >= 2 && fields[0].toULongOrNull(16) == peerId) fields[1] else null
        }?.takeIf { it.isNotBlank() } ?: "The other player"

        if (phase == "incoming" && answeredEpoch != epoch)
            return AmbientHandoffScreen.IncomingOffer(epoch, mode, peerName)
        if (phase == "incoming" && answeredEpoch == epoch && !acceptedAnswer) return null
        if (phase == "missing_rom") return AmbientHandoffScreen.MissingRom(epoch, mode)
        val progress = when (phase) {
            "incoming", "outgoing" -> AmbientHandoffPhase.WAITING_PEERS
            "accepted", "rom_ready", "fencing" -> AmbientHandoffPhase.FENCING
            "transferring" -> AmbientHandoffPhase.TRANSFERRING
            "verifying" -> AmbientHandoffPhase.VERIFYING
            "committing" -> AmbientHandoffPhase.COMMITTING
            "rolling_back" -> AmbientHandoffPhase.ROLLING_BACK
            else -> return AmbientHandoffScreen.Unresolved("Unknown native handoff phase: $phase")
        }
        return AmbientHandoffScreen.Progress(
            epoch = epoch,
            mode = mode,
            phase = progress,
            peersAccepted = if (phase == "incoming" || phase == "outgoing") 1 else 2,
            peersRequired = 2,
        )
    }

    private fun counters(stats: String): Map<String, String> = stats.split(' ')
        .mapNotNull { item ->
            val equals = item.indexOf('=')
            if (equals > 0) item.substring(0, equals) to item.substring(equals + 1) else null
        }.toMap()
}
