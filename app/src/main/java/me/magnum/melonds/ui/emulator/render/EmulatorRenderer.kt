package me.magnum.melonds.ui.emulator.render

import me.magnum.melonds.domain.model.render.PresentFrameWrapper
import me.magnum.melonds.ui.emulator.model.RuntimeRendererConfiguration

interface EmulatorRenderer {
    fun updateRendererConfiguration(newRendererConfiguration: RuntimeRendererConfiguration?)
    fun setLeftRotationEnabled(enabled: Boolean)
    fun onSurfaceCreated()
    fun onSurfaceChanged(width: Int, height: Int)
    fun drawFrame(presentFrameWrapper: PresentFrameWrapper)
    // non-null: this frame would look exactly like any other frame with an equal key (no DS
    // screen on this surface), so a surface that already shows it can skip the draw + swap
    fun staticFrameKey(presentFrameWrapper: PresentFrameWrapper): Any? = null
}