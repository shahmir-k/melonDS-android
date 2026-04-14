package me.magnum.melonds.debug

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.domain.model.Input
import kotlin.concurrent.thread

class EmulatorDebugReceiver : BroadcastReceiver() {
    companion object {
        const val ACTION = "me.magnum.melonds.DEBUG_EMULATOR"

        private const val EXTRA_LOAD_STATE_URI = "load_state_uri"
        private const val EXTRA_SEQUENCE = "sequence"
        private const val EXTRA_PRESS_MS = "press_ms"
        private const val EXTRA_GAP_MS = "gap_ms"
        private const val EXTRA_FAST_FORWARD = "fast_forward"

        private const val TAG = "EmulatorDebugReceiver"
    }

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION) return

        val pendingResult = goAsync()
        thread(name = "EmuDebugReceiver") {
            try {
                val loadStateUri = intent.getStringExtra(EXTRA_LOAD_STATE_URI)
                if (!loadStateUri.isNullOrBlank()) {
                    val loaded = MelonEmulator.loadState(Uri.parse(loadStateUri))
                    Log.i(TAG, "loadState($loadStateUri) -> $loaded")
                }

                if (intent.hasExtra(EXTRA_FAST_FORWARD)) {
                    val enabled = intent.getBooleanExtra(EXTRA_FAST_FORWARD, false)
                    MelonEmulator.setFastForwardEnabled(enabled)
                    Log.i(TAG, "setFastForwardEnabled($enabled)")
                }

                val sequence = intent.getStringExtra(EXTRA_SEQUENCE)?.trim().orEmpty()
                if (sequence.isNotEmpty()) {
                    val pressMs = intent.getLongExtra(EXTRA_PRESS_MS, 80L).coerceAtLeast(0L)
                    val gapMs = intent.getLongExtra(EXTRA_GAP_MS, 180L).coerceAtLeast(0L)
                    runSequence(sequence, pressMs, gapMs)
                }
            } catch (t: Throwable) {
                Log.e(TAG, "Debug harness failed", t)
            } finally {
                pendingResult.finish()
            }
        }
    }

    private fun runSequence(sequence: String, pressMs: Long, gapMs: Long) {
        val commands = sequence
            .split(',')
            .map { it.trim() }
            .filter { it.isNotEmpty() }

        for (command in commands) {
            when {
                command.startsWith("SLEEP:", ignoreCase = true) -> {
                    val duration = command.substringAfter(':', "0").trim().toLongOrNull()?.coerceAtLeast(0L) ?: 0L
                    Thread.sleep(duration)
                }

                command.startsWith("TOUCH:", ignoreCase = true) -> {
                    val coords = command.substringAfter(':').split(':')
                    if (coords.size == 2) {
                        val x = coords[0].trim().toIntOrNull()
                        val y = coords[1].trim().toIntOrNull()
                        if (x != null && y != null) {
                            MelonEmulator.onScreenTouch(x, y)
                            Thread.sleep(pressMs)
                            MelonEmulator.onScreenRelease()
                            Thread.sleep(gapMs)
                        }
                    }
                }

                else -> {
                    val input = Input.entries.firstOrNull { it.name.equals(command, ignoreCase = true) }
                    if (input == null) {
                        Log.w(TAG, "Unknown debug input command: $command")
                        continue
                    }

                    MelonEmulator.onInputDown(input)
                    Thread.sleep(pressMs)
                    MelonEmulator.onInputUp(input)
                    Thread.sleep(gapMs)
                }
            }
        }
    }
}
