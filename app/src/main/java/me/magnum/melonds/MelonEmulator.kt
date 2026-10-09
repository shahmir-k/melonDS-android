package me.magnum.melonds

import android.net.Uri
import me.magnum.melonds.common.camera.DSiCameraSource
import me.magnum.melonds.domain.model.Cheat
import me.magnum.melonds.domain.model.EmulatorConfiguration
import me.magnum.melonds.domain.model.Input
import me.magnum.melonds.domain.model.retroachievements.RASimpleAchievement
import me.magnum.melonds.domain.model.retroachievements.RASimpleLeaderboard
import me.magnum.melonds.domain.model.retroachievements.RASimpleRuntimeAchievement
import me.magnum.melonds.ui.emulator.render.FrameRenderCallback
import me.magnum.melonds.ui.emulator.rewind.model.RewindSaveState
import me.magnum.melonds.ui.emulator.rewind.model.RewindWindow
import java.nio.ByteBuffer

object MelonEmulator {
    enum class LoadResult(val isTerminal: Boolean) {
        SUCCESS(false),
        SUCCESS_GBA_FAILED(false),
        NDS_FAILED(true),
        BIOS_FAILED(true)
    }

    enum class FirmwareLoadResult {
        SUCCESS,
        BIOS9_MISSING,
        BIOS9_BAD,
        BIOS7_MISSING,
        BIOS7_BAD,
        FIRMWARE_MISSING,
        FIRMWARE_BAD,
        FIRMWARE_NOT_BOOTABLE,
        DSI_BIOS9_MISSING,
        DSI_BIOS9_BAD,
        DSI_BIOS7_MISSING,
        DSI_BIOS7_BAD,
        DSI_NAND_MISSING,
        DSI_NAND_BAD
    }

    enum class GbaSlotType {
        NONE,
        GBA_ROM,
        RUMBLE_PAK,
        MEMORY_EXPANSION,
    }

	external fun setupEmulator(
        emulatorConfiguration: EmulatorConfiguration,
        dsiCameraSource: DSiCameraSource?,
        screenshotBuffer: ByteBuffer,
    )

    external fun setupCheats(cheats: Array<Cheat>)

    external fun setupAchievements(achievements: Array<RASimpleAchievement>, leaderboards: Array<RASimpleLeaderboard>, richPresenceScript: String?)

    external fun unloadRetroAchievementsData()

    external fun getRichPresenceStatus(): String?

    external fun getRuntimeAchievements(): Array<RASimpleRuntimeAchievement>

	fun loadRom(romUri: Uri, sramUri: Uri, gbaSlotType: GbaSlotType, gbaRomUri: Uri?, gbaSramUri: Uri?): LoadResult {
        val loadResult = loadRomInternal(romUri.toString(), sramUri.toString(), gbaSlotType.ordinal, gbaRomUri?.toString(), gbaSramUri?.toString())
        return when (loadResult) {
            0 -> LoadResult.SUCCESS
            1 -> LoadResult.SUCCESS_GBA_FAILED
            2 -> LoadResult.NDS_FAILED
            3 -> LoadResult.BIOS_FAILED
            else -> throw RuntimeException("Unknown load result")
        }
    }

    fun bootFirmware(): FirmwareLoadResult {
        val loadResult = bootFirmwareInternal()
        return FirmwareLoadResult.entries[loadResult]
    }

    private external fun loadRomInternal(romPath: String, sramPath: String, gbaSlotType: Int, gbaRomPath: String?, gbaSramPath: String?): Int

    private external fun bootFirmwareInternal(): Int

	external fun startEmulation()

    external fun presentFrame(deadlineNs: Long, frameRenderCallback: FrameRenderCallback)

	external fun getFPS(): Float

	external fun pauseEmulation()

	external fun resumeEmulation()

    external fun resetEmulation()

	external fun stopEmulation()

    // LAN multiplayer (MelonDSAndroidJNI.cpp). Only acts while emulation is paused: the native
    // side parks the emulator thread around every call; otherwise these return false / empty.
    external fun lanGetMode(): Int
    external fun lanHost(playerName: String, maxPlayers: Int): Boolean
    external fun lanStartDiscovery(): Boolean
    external fun lanGetSessions(): Array<String>
    external fun lanJoin(playerName: String, hostAddress: String): Boolean
    external fun lanGetPlayers(): Array<String>
    external fun lanTick()
    external fun lanLeave()

    // Group: the Netplay lobby kept connected while its session runs, so the host's commands
    // (mode switch, Hosted server change, end) reach everyone. Commands: (server << 16) |
    // (mode << 8) | players; mode 0 Netplay, 1 Hosted, 2 LAN, 3 end the session.
    external fun lanToGroup(): Boolean
    external fun groupToLan(): Boolean
    external fun groupPlayers(): Array<String>
    external fun groupTake(): Int
    external fun groupSend(mode: Int, players: Int, server: Int)
    external fun groupLeave()
    external fun lanStartSession(hosted: Boolean, players: Int, server: Int)
    external fun lanGetStartRequest(): Int
    // The next game started runs in Netplay (every player's console on every device) as `player`
    // (the LAN lobby id, 0 = host) of `players`; guests pass the host's IP as `host`, the host "".
    // hosted: Hosted Netplay (the host runs every console, each guest only its own)
    // library: ROM paths/URIs where another player's console finds that player's game (cross-game);
    // cacheDir: where a game received from another player goes (cacheMaxBytes: its size cap)
    external fun netplayPrepare(player: Int, players: Int, host: String, hosted: Boolean, library: Array<String>,
                                cacheDir: String, cacheMaxBytes: Long)
    // Session setup's ROM transfer, polled by the UI: "state\nbytes\ntotal\ntitle\nquestion"
    // (state: NetplayTransfer.STATE_*)
    external fun netplayTransferStatus(): String
    // the answer to the transfer's consent question
    external fun netplayAnswer(yes: Boolean)
    // ends session setup (and a ROM transfer) on this device
    external fun netplayCancelSetup()
    // "" when not in Netplay, else "Netplay", "waiting for other player" or "DESYNC"
    external fun netplayStatus(): String

    // 0 = no Netplay session, 1 = Netplay, 2 = Hosted Netplay
    external fun netplayKind(): Int

    fun saveState(path: Uri): Boolean {
        return saveStateInternal(path.toString())
    }

    private external fun saveStateInternal(path: String): Boolean

    fun loadState(path: Uri): Boolean {
        return loadStateInternal(path.toString())
    }

    private external fun loadStateInternal(path: String): Boolean

    external fun loadRewindState(rewindSaveState: RewindSaveState): Boolean

    external fun getRewindWindow(): RewindWindow

	external fun onScreenTouch(x: Int, y: Int)

	external fun onScreenRelease()

	fun onInputDown(input: Input) {
        onKeyPress(input.keyCode)
    }

	fun onInputUp(input: Input) {
        onKeyRelease(input.keyCode)
    }

    private external fun onKeyPress(key: Int)

    private external fun onKeyRelease(key: Int)

    external fun takeScreenshot(): Boolean

    external fun setFastForwardEnabled(enabled: Boolean)

    external fun setMicrophoneEnabled(enabled: Boolean)

    external fun updateEmulatorConfiguration(emulatorConfiguration: EmulatorConfiguration)
}