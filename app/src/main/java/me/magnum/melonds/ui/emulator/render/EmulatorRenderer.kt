package me.magnum.melonds.ui.emulator.render

import me.magnum.melonds.domain.model.render.PresentFrameWrapper
import me.magnum.melonds.ui.emulator.model.RuntimeRendererConfiguration

interface EmulatorRenderer {
    fun updateRendererConfiguration(newRendererConfiguration: RuntimeRendererConfiguration?)
    fun setLeftRotationEnabled(enabled: Boolean)
    fun onSurfaceCreated()
    fun onSurfaceChanged(width: Int, height: Int)
    fun drawFrame(presentFrameWrapper: PresentFrameWrapper)
    // non-null: drawing this frame gives exactly the pixels of any earlier frame with an equal
    // key, so a surface that already shows that frame can skip the draw and the swap
    fun frameKey(presentFrameWrapper: PresentFrameWrapper): Any? = null
}