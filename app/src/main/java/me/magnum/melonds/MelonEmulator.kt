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

    // debug.litev.<name> as an int (def when unset)
    external fun litevProp(name: String, def: Int): Int

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

    // Just Play local wireless (LITEV_AMBIENT_LAN; AmbientWireless.kt). mode: 0 everyone nearby, 1 only while the
    // game's wireless is on, 2 hidden, 3 lobby only (off). Configure before a game starts.
    external fun ambientConfigure(deviceId: Long, name: String, mode: Int)
    external fun ambientSetForeground(foreground: Boolean)
    external fun ambientAddPeer(ip: String, port: Int)
    external fun ambientPort(): Int
    external fun ambientNearby(): Array<String>     // "id \t name \t game \t ip:port \t ggid \t build \t linked"
    external fun ambientStats(): String             // "key=value ..." counters (cumulative), "" = not running
    external fun ambientDismissSlowdownPrompt()      // keep LAN; the detector will not offer again this link session
    external fun ambientOfferHandoff(mode: Int): Boolean // 1 plain Netplay, 2 Hosted; true = request queued
    external fun ambientRespondHandoff(epoch: Long, accept: Boolean): Boolean
    external fun ambientCancelHandoff(): Boolean
    external fun ambientHandoffStatus(): String // "phase\tepoch\tmode\tpeer_id"; phase: none/incoming/outgoing/accepted/closed/failed

    // Group: the Netplay lobby kept connected while its session runs, so the host's commands
    // (mode switch, Hosted server change, end) reach everyone. Commands: (server << 16) |
    // (mode << 8) | players; mode 0 Netplay, 1 Hosted, 2 LAN, 3 end the session.
    external fun lanToGroup(): Boolean
    external fun groupToLan(): Boolean
    external fun groupPlayers(): Array<String>
    external fun groupTake(): Int
    external fun groupLeft(): Array<String>     // players whose group link closed: "id \t name"
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
    // Netplay (plain and Hosted): the fast-forward key toggles ONE session speed for every player.
    // Both return what the key shows (1 fast, 0 normal), -1 = not in Netplay (the key is local).
    external fun netplayToggleFastForward(): Int
    external fun netplayFastForwardShown(): Int
    // the session speed: "" normal, else "FF 2x by P2" / "uncapped by P1"
    external fun netplaySpeedLabel(): String

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

    // Record mode: "record DIR", "replay DIR" or "stop"; runs at the next emulated frame
    external fun recordQueue(request: String)

    external fun recordMode(): Int     // 0 none, 1 recording, 2 replaying

    // the host battery the guest sees in single player; percent < 0 = always full (Netplay, LAN
    // and record mode always pin full natively)
    external fun setHostBattery(percent: Int, charging: Boolean)

    external fun recordMark(): Int     // flag this moment for investigation; the frame, -1 when not recording

    external fun takeScreenshot(): Boolean

    external fun setFastForwardEnabled(enabled: Boolean)

    external fun setMicrophoneEnabled(enabled: Boolean)

    external fun updateEmulatorConfiguration(emulatorConfiguration: EmulatorConfiguration)
}
