package me.magnum.melonds.ui.emulator.render

import me.magnum.melonds.domain.model.render.PresentFrameWrapper
import me.magnum.melonds.ui.emulator.model.RuntimeRendererConfiguration

interface EmulatorRenderer {
    fun updateRendererConfiguration(newRendererConfiguration: RuntimeRendererConfiguration?)
    fun setLeftRotationEnabled(enabled: Boolean)
    fun onSurfaceCreated()
    fun onSurfaceChanged(width: Int, height: Int)
    fun drawFrame(presentFrameWrapper: PresentFrameWrapper)
    // LITEV_HYB_DIRECT_PRESENT: the screens' quads (x, y, u, v, alpha per vertex) when the frame
    // can be drawn as is (default screen shader, no background or overlay), else null
    fun directPresentLayout(): DirectPresentLayout? = null
}

class DirectPresentLayout(val vertices: FloatArray, val linear: Boolean, val width: Int, val height: Int, val version: Int)
