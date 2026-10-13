package me.magnum.melonds.ui.emulator.multiplayer

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.padding
import androidx.compose.material.CircularProgressIndicator
import androidx.compose.material.MaterialTheme
import androidx.compose.material.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import me.magnum.melonds.R
import me.magnum.melonds.ui.common.component.dialog.BaseDialog
import me.magnum.melonds.ui.common.component.dialog.DialogButton

/** UI-only representation. The native handoff protocol owns the session and its source checkpoint. */
enum class AmbientHandoffMode { NETPLAY, HOSTED_NETPLAY }

enum class AmbientHandoffPhase {
    OFFERED, WAITING_PEERS, FENCING, TRANSFERRING, VERIFYING, COMMITTING, ROLLING_BACK
}

sealed interface AmbientHandoffScreen {
    /** The game is still using LAN; choosing a mode starts an offer, not an immediate backend switch. */
    data class SlowLan(val fps: Int) : AmbientHandoffScreen

    /** A linked peer has offered to switch. This peer must independently consent. */
    data class IncomingOffer(val epoch: Long, val mode: AmbientHandoffMode, val peerName: String) : AmbientHandoffScreen

    data class Progress(
        val epoch: Long,
        val mode: AmbientHandoffMode,
        val phase: AmbientHandoffPhase,
        val peersAccepted: Int,
        val peersRequired: Int,
    ) : AmbientHandoffScreen {
        /** Cancellation is a rollback request. It must never leave one peer in the new backend. */
        val canCancel: Boolean get() = phase != AmbientHandoffPhase.COMMITTING && phase != AmbientHandoffPhase.ROLLING_BACK
    }

    /** The native side has rolled back to LAN before this is shown. */
    data class Failed(val reason: String) : AmbientHandoffScreen
}

/**
 * Presentation for the live, automatic Just Play handoff. Deliberately has no JNI calls: the
 * controller must only show it once the native two-peer commit/rollback protocol is proven and
 * exposes a stable status API. In particular, the existing restart-based `startNetplay` path must
 * not be used for these callbacks.
 */
@Composable
fun AmbientHandoffDialog(
    screen: AmbientHandoffScreen,
    onKeepLan: () -> Unit,
    onOffer: (AmbientHandoffMode) -> Unit,
    onRespond: (epoch: Long, accept: Boolean) -> Unit,
    onCancel: (epoch: Long) -> Unit,
    onDismissFailure: () -> Unit,
) {
    when (screen) {
        is AmbientHandoffScreen.SlowLan -> BaseDialog(
            title = stringResource(R.string.ambient_handoff_slow_title),
            onDismiss = onKeepLan,
            content = { padding ->
                DialogText(padding, stringResource(R.string.ambient_handoff_slow_body, screen.fps))
            },
            buttons = {
                DialogButton(stringResource(R.string.ambient_handoff_keep_lan), onClick = onKeepLan)
                DialogButton(stringResource(R.string.multiplayer_option_netplay), onClick = { onOffer(AmbientHandoffMode.NETPLAY) })
                DialogButton(stringResource(R.string.multiplayer_option_hosted), onClick = { onOffer(AmbientHandoffMode.HOSTED_NETPLAY) })
            },
        )

        is AmbientHandoffScreen.IncomingOffer -> BaseDialog(
            title = stringResource(R.string.ambient_handoff_offer_title),
            onDismiss = { onRespond(screen.epoch, false) },
            content = { padding ->
                DialogText(
                    padding,
                    stringResource(
                        R.string.ambient_handoff_offer_body,
                        screen.peerName,
                        modeName(screen.mode),
                    ),
                )
            },
            buttons = {
                DialogButton(stringResource(R.string.ambient_handoff_decline), onClick = { onRespond(screen.epoch, false) })
                DialogButton(stringResource(R.string.ambient_handoff_accept), onClick = { onRespond(screen.epoch, true) })
            },
        )

        is AmbientHandoffScreen.Progress -> BaseDialog(
            title = stringResource(R.string.ambient_handoff_progress_title),
            onDismiss = { if (screen.canCancel) onCancel(screen.epoch) },
            content = { padding ->
                Column(Modifier.padding(padding)) {
                    Text(stringResource(R.string.ambient_handoff_progress_mode, modeName(screen.mode)))
                    Text(phaseName(screen.phase), modifier = Modifier.padding(top = 8.dp))
                    if (screen.phase == AmbientHandoffPhase.WAITING_PEERS) {
                        Text(stringResource(R.string.ambient_handoff_accept_count, screen.peersAccepted, screen.peersRequired))
                    }
                    CircularProgressIndicator(Modifier.padding(top = 12.dp))
                }
            },
            buttons = {
                if (screen.canCancel) {
                    DialogButton(stringResource(R.string.ambient_handoff_cancel), onClick = { onCancel(screen.epoch) })
                }
            },
        )

        is AmbientHandoffScreen.Failed -> BaseDialog(
            title = stringResource(R.string.ambient_handoff_failed_title),
            onDismiss = onDismissFailure,
            content = { padding ->
                Column(Modifier.padding(padding)) {
                    Text(stringResource(R.string.ambient_handoff_failed_body))
                    Text(screen.reason, color = MaterialTheme.colors.error, modifier = Modifier.padding(top = 8.dp))
                }
            },
            buttons = {
                DialogButton(stringResource(R.string.ambient_handoff_keep_lan), onClick = onDismissFailure)
            },
        )
    }
}

@Composable
private fun DialogText(padding: PaddingValues, text: String) {
    Text(text, modifier = Modifier.padding(padding))
}

@Composable
private fun modeName(mode: AmbientHandoffMode): String = stringResource(
    when (mode) {
        AmbientHandoffMode.NETPLAY -> R.string.multiplayer_option_netplay
        AmbientHandoffMode.HOSTED_NETPLAY -> R.string.multiplayer_option_hosted
    },
)

@Composable
private fun phaseName(phase: AmbientHandoffPhase): String = stringResource(
    when (phase) {
        AmbientHandoffPhase.OFFERED, AmbientHandoffPhase.WAITING_PEERS -> R.string.ambient_handoff_waiting
        AmbientHandoffPhase.FENCING -> R.string.ambient_handoff_fencing
        AmbientHandoffPhase.TRANSFERRING -> R.string.ambient_handoff_transferring
        AmbientHandoffPhase.VERIFYING -> R.string.ambient_handoff_verifying
        AmbientHandoffPhase.COMMITTING -> R.string.ambient_handoff_committing
        AmbientHandoffPhase.ROLLING_BACK -> R.string.ambient_handoff_rolling_back
    },
)
