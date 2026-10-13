package me.magnum.melonds.ui.emulator

import android.util.Log
import me.magnum.melonds.MelonEmulator

// debug.litev.presenttime=1: per-step CPU time of the present (FrameRenderThread), logged every
// 120 presents (LITEV_PRESENTTIME, microseconds per present): makeCurrent, clear, background,
// screen state, screen draw, overlay, swap
object PresentTimer {
    private val on by lazy { MelonEmulator.litevProp("presenttime", 0) != 0 }
    private val t = LongArray(10)
    private val sum = LongArray(7)
    private var n = 0

    fun mark(i: Int) { if (on) t[i] = System.nanoTime() }

    fun frame() {
        if (!on) return
        sum[0] += t[7] - t[6]; sum[1] += t[1] - t[0]; sum[2] += t[2] - t[1]; sum[3] += t[3] - t[2]
        sum[4] += t[4] - t[3]; sum[5] += t[5] - t[4]; sum[6] += t[9] - t[8]
        if (++n == 120) {
            Log.i("LITEV_PRESENTTIME", "120 presents, us each: makeCurrent %d clear %d background %d screenState %d screenDraw %d overlay %d swap %d".format(
                *sum.map { it / 120 / 1000 }.toTypedArray()))
            sum.fill(0); n = 0
        }
    }
}
