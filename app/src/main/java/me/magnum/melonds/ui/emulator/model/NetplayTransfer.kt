package me.magnum.melonds.ui.emulator.model

/** Netplay session setup's ROM transfer, as native reports it (MelonEmulator.netplayTransferStatus). */
data class NetplayTransfer(val state: Int, val bytes: Long, val total: Long, val title: String, val question: String) {
    val active get() = state == STATE_ASKING || state == STATE_SENDING || state == STATE_RECEIVING

    companion object {
        // NetplayXferStatus (melonDS NetplayInput.h)
        const val STATE_IDLE = 0
        const val STATE_ASKING = 1
        const val STATE_SENDING = 2
        const val STATE_RECEIVING = 3

        fun parse(status: String): NetplayTransfer {
            val f = status.split('\n', limit = 5)
            return NetplayTransfer(
                f.getOrNull(0)?.toIntOrNull() ?: STATE_IDLE,
                f.getOrNull(1)?.toLongOrNull() ?: 0,
                f.getOrNull(2)?.toLongOrNull() ?: 0,
                f.getOrNull(3).orEmpty(),
                f.getOrNull(4).orEmpty().trim(),
            )
        }
    }
}
