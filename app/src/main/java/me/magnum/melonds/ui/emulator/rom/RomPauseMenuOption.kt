package me.magnum.melonds.ui.emulator.rom

import me.magnum.melonds.R
import me.magnum.melonds.ui.emulator.PauseMenuOption

enum class RomPauseMenuOption(override val textResource: Int) : PauseMenuOption {
    SETTINGS(R.string.settings),
    SAVE_STATE(R.string.save_state),
    LOAD_STATE(R.string.load_state),
    REWIND(R.string.rewind),
    RECORD(R.string.record_start),
    REPLAY(R.string.record_replay),
    SHARE_RECORDING(R.string.record_share),
    CHEATS(R.string.cheats),
    MULTIPLAYER(R.string.multiplayer_role_title),
    VIEW_ACHIEVEMENTS(R.string.achievements),
    RESET(R.string.reset),
    EXIT(R.string.exit)
}