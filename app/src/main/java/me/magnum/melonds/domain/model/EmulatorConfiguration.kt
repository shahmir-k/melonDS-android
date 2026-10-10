package me.magnum.melonds.domain.model

import android.net.Uri

data class EmulatorConfiguration(
        val useCustomBios: Boolean,
        val showBootScreen: Boolean,
        val dsBios7Uri: Uri?,
        val dsBios9Uri: Uri?,
        val dsFirmwareUri: Uri?,
        val dsiBios7Uri: Uri?,
        val dsiBios9Uri: Uri?,
        val dsiFirmwareUri: Uri?,
        val dsiNandUri: Uri?,
        val internalDirectory: String,
        val fastForwardSpeedMultiplier: Float,
        val rewindEnabled: Boolean,
        val rewindPeriodSeconds: Int,
        val rewindWindowSeconds: Int,
        val useJit: Boolean,
        val consoleType: ConsoleType,
        val soundEnabled: Boolean,
        val audioInterpolation: AudioInterpolation,
        val audioBitrate: AudioBitrate,
        val volume: Int,
        val audioLatency: AudioLatency,
        val micSource: MicSource,
        val firmwareConfiguration: FirmwareConfiguration,
        val rendererConfiguration: RendererConfiguration,
        val autoFrameskipEnabled: Boolean = false,
        val fastForwardMaxFrameskip: Int = 0,
        // debug.litev.skiprepeat values: 3 auto, 2 always, 1 off (Netplay/LAN keep it on)
        val skipRepeatMode: Int = 2,
        // Audio quality: 1 full, 2 balanced (mix at half rate), 4 performance (quarter rate)
        val audioQualityDiv: Int = 4
)