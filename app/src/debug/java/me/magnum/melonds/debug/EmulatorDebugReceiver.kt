package me.magnum.melonds.debug

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.domain.model.Input
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread

class EmulatorDebugReceiver : BroadcastReceiver() {
    companion object {
        const val ACTION = "me.magnum.melonds.DEBUG_EMULATOR"

        private const val EXTRA_LOAD_STATE_URI = "load_state_uri"
        private const val EXTRA_SEQUENCE = "sequence"
        private const val EXTRA_PRESS_MS = "press_ms"
        private const val EXTRA_GAP_MS = "gap_ms"
        private const val EXTRA_FAST_FORWARD = "fast_forward"
        private const val EXTRA_FPS_SAMPLE_COUNT = "fps_sample_count"
        private const val EXTRA_FPS_INTERVAL_MS = "fps_interval_ms"
        private const val EXTRA_SAMPLE_TOKEN = "sample_token"
        private const val EXTRA_CANCEL_SEQUENCE = "cancel_sequence"

        private const val TAG = "EmulatorDebugReceiver"

        private val sequenceGeneration = AtomicInteger(0)
    }

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION) return

        val pendingResult = goAsync()
        thread(name = "EmuDebugReceiver") {
            try {
                if (intent.getBooleanExtra(EXTRA_CANCEL_SEQUENCE, false)) {
                    cancelActiveSequence()
                    return@thread
                }

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
                    runSequence(sequence, pressMs, gapMs, startNewSequence())
                }

                val fpsSampleCount = intent.getIntExtra(EXTRA_FPS_SAMPLE_COUNT, 0).coerceAtLeast(0)
                if (fpsSampleCount > 0) {
                    val fpsIntervalMs = intent.getLongExtra(EXTRA_FPS_INTERVAL_MS, 1000L).coerceAtLeast(1L)
                    val sampleToken = intent.getStringExtra(EXTRA_SAMPLE_TOKEN)?.ifBlank { null }
                    sampleFps(fpsSampleCount, fpsIntervalMs, sampleToken)
                }
            } catch (t: Throwable) {
                Log.e(TAG, "Debug harness failed", t)
            } finally {
                pendingResult.finish()
            }
        }
    }

    private fun runSequence(sequence: String, pressMs: Long, gapMs: Long, generation: Int) {
        val commands = sequence
            .split(',')
            .map { it.trim() }
            .filter { it.isNotEmpty() }

        try {
            for (command in commands) {
                ensureSequenceActive(generation)
                when {
                    command.startsWith("SLEEP:", ignoreCase = true) -> {
                        val duration = command.substringAfter(':', "0").trim().toLongOrNull()?.coerceAtLeast(0L) ?: 0L
                        sleepCancellable(duration, generation)
                    }

                    command.startsWith("TOUCH:", ignoreCase = true) -> {
                        val coords = command.substringAfter(':').split(':')
                        if (coords.size == 2) {
                            val x = coords[0].trim().toIntOrNull()
                            val y = coords[1].trim().toIntOrNull()
                            if (x != null && y != null) {
                                MelonEmulator.onScreenTouch(x, y)
                                sleepCancellable(pressMs, generation)
                                MelonEmulator.onScreenRelease()
                                sleepCancellable(gapMs, generation)
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
                        try {
                            sleepCancellable(pressMs, generation)
                        } finally {
                            MelonEmulator.onInputUp(input)
                        }
                        sleepCancellable(gapMs, generation)
                    }
                }
            }
        } finally {
            releaseAllInputs()
        }
    }

    private fun sampleFps(sampleCount: Int, intervalMs: Long, token: String?) {
        val samples = mutableListOf<Float>()
        repeat(sampleCount) { index ->
            val fps = MelonEmulator.getFPS()
            samples += fps
            if (index + 1 < sampleCount) {
                Thread.sleep(intervalMs)
            }
        }

        val avg = if (samples.isEmpty()) 0f else samples.sum() / samples.size
        val tokenSuffix = token?.let { " token=$it" }.orEmpty()
        Log.i(
            TAG,
            "HARNESS_FPS$tokenSuffix avg=${"%.3f".format(avg)} " +
                "samples=${samples.joinToString(prefix = "[", postfix = "]") { "%.3f".format(it) }}"
        )
    }

    private fun startNewSequence(): Int {
        val generation = sequenceGeneration.incrementAndGet()
        releaseAllInputs()
        Log.i(TAG, "Starting debug input sequence generation=$generation")
        return generation
    }

    private fun cancelActiveSequence() {
        val generation = sequenceGeneration.incrementAndGet()
        releaseAllInputs()
        Log.i(TAG, "Cancelled active debug input sequence generation=$generation")
    }

    private fun ensureSequenceActive(generation: Int) {
        if (sequenceGeneration.get() != generation) {
            throw CancellationException()
        }
    }

    private fun sleepCancellable(durationMs: Long, generation: Int) {
        var remaining = durationMs
        while (remaining > 0) {
            ensureSequenceActive(generation)
            val chunk = minOf(remaining, 50L)
            Thread.sleep(chunk)
            remaining -= chunk
        }
    }

    private fun releaseAllInputs() {
        Input.SYSTEM_BUTTONS.forEach(MelonEmulator::onInputUp)
        MelonEmulator.onScreenRelease()
    }

    private class CancellationException : RuntimeException()
}
