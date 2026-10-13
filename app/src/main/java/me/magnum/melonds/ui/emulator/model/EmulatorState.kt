package me.magnum.melonds.ui.emulator.model

import android.net.Uri
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.domain.model.ConsoleType
import me.magnum.melonds.domain.model.rom.Rom

sealed class EmulatorState {
    data object Uninitialized : EmulatorState()
    data class ValidatingRom(val rom: Rom) : EmulatorState()
    data object LoadingRom : EmulatorState()
    data class ValidatingFirmware(val consoleType: ConsoleType) : EmulatorState()
    data object LoadingFirmware : EmulatorState()
    data class RunningRom(val rom: Rom) : EmulatorState()
    data class RunningFirmware(val console: ConsoleType) : EmulatorState()
    data object RomLoadError : EmulatorState()
    data class FirmwareLoadError(val reason: MelonEmulator.FirmwareLoadResult) : EmulatorState()
    data class RomNotFoundError(val romPath: String) : EmulatorState()
    data class RomStoragePermissionRequired(val romUri: Uri, val rom: Rom?) : EmulatorState()

    fun isRunning() = this is RunningRom || this is RunningFirmware
}
