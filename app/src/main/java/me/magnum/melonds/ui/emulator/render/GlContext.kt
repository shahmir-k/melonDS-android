package me.magnum.melonds.ui.emulator.render

import me.magnum.melonds.MelonEmulator

import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.view.Surface

class GlContext(sharedEglContext: Long? = null) {

    private var display: EGLDisplay
    private var config: EGLConfig
    private var context: Long

    init {
        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        if (display == EGL14.EGL_NO_DISPLAY) {
            throw GlContextException("No display")
        }

        val version = IntArray(2)
        if (!EGL14.eglInitialize(display, version, 0, version, 1)) {
            throw GlContextException("Unable to initialize EGL")
        }

        config = createGlConfig()
        context = createContext(display.nativeHandle, config.nativeHandle, sharedEglContext ?: 0)
        if (context == 0L) {
            throw GlContextException("Failed to create context: ${EGL14.eglGetError()}")
        }
    }

    // debug.litev.presentlean bit 1 (default on): make a surface current and set its swap interval only
    // when it changes (one surface per present on most devices), not on every present
    private val lean by lazy { (MelonEmulator.litevProp("presentlean", 3) and 1) != 0 }
    private var currentSurface: EGLSurface? = null

    fun use(surface: EGLSurface) {
        if (lean && surface == currentSurface) return
        if (!makeCurrent(display.nativeHandle, surface.nativeHandle, context)) {
            currentSurface = null
            throw GlContextException("Failed to make current: ${EGL14.eglGetError()}")
        }
        currentSurface = surface

        // Allows immediate buffer swapping without waiting for VSync
        // (debug.litev.presentlean bit 8: interval 1, diagnostic A/B of the swap cost)
        EGL14.eglSwapInterval(display, if ((MelonEmulator.litevProp("presentlean", 3) and 8) != 0) 1 else 0)
    }

    fun swapBuffers(surface: EGLSurface) {
        EGL14.eglSwapBuffers(display, surface)
    }

    fun release() {
        currentSurface = null
        EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
    }

    fun destroy() {
        destroyContext(display.nativeHandle, context)
        EGL14.eglTerminate(display)
        EGL14.eglReleaseThread()

        display = EGL14.EGL_NO_DISPLAY
        context = 0L
    }

    fun createWindowSurface(surface: Surface): EGLSurface {
        val eglSurface = EGL14.eglCreateWindowSurface(display, config, surface, intArrayOf(EGL14.EGL_NONE), 0)
        if (eglSurface == EGL14.EGL_NO_SURFACE) {
            throw GlContextException("Failed to create window surface: ${EGL14.eglGetError()}")
        }

        return eglSurface
    }

    fun destroyWindowSurface(eglSurface: EGLSurface) {
        if (eglSurface == currentSurface) {
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            currentSurface = null
        }
        EGL14.eglDestroySurface(display, eglSurface)
    }

    private fun createGlConfig(): EGLConfig {
        val attributeList = intArrayOf(
            EGL14.EGL_RENDERABLE_TYPE, EGLExt.EGL_OPENGL_ES3_BIT_KHR,
            EGL14.EGL_RED_SIZE, 8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_DEPTH_SIZE, 24,
            EGL14.EGL_STENCIL_SIZE, 8,
            EGL14.EGL_NONE,
        )

        val eglConfig = arrayOfNulls<EGLConfig?>(1)
        val numConfigs = IntArray(1)
        if (!EGL14.eglChooseConfig(display, attributeList, 0, eglConfig, 0, 1, numConfigs, 0)) {
            throw GlContextException("Unable to choose config")
        }

        return eglConfig[0]!!
    }

    // Because we need to use a shared EGL context, and we can't build an EGLContext instance from a native context instance, we need to perform context manipulation
    // operations on the native side instead of using the Java wrappers

    private external fun createContext(display: Long, config: Long, sharedGlContext: Long): Long
    private external fun makeCurrent(display: Long, surface: Long, context: Long): Boolean
    private external fun destroyContext(display: Long, context: Long)

    class GlContextException(message: String) : Exception(message)
}