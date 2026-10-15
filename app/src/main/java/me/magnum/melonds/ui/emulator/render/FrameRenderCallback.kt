package me.magnum.melonds.ui.emulator.render

fun interface FrameRenderCallback {
    // screenSerials: per DS screen (top high 32 bits, bottom low) the renderer's serial of its
    // picture; equal = identical pixels, 0 = unknown
    fun renderFrame(isValidFrame: Boolean, frameTextureId: Int, screenSerials: Long)
}