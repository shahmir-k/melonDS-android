package me.magnum.melonds.ui.emulator

import android.content.Context
import android.opengl.EGLSurface
import android.opengl.GLES30
import android.util.AttributeSet
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.domain.model.render.PresentFrameWrapper
import me.magnum.melonds.ui.emulator.model.RuntimeRendererConfiguration
import me.magnum.melonds.ui.emulator.render.EmulatorRenderer
import me.magnum.melonds.ui.emulator.render.GlContext

class EmulatorSurfaceView(context: Context, attrs: AttributeSet? = null) : SurfaceView(context, attrs), SurfaceHolder.Callback {

    private val surfaceLock = Object()
    private var surfaceWidth = 0
    private var surfaceHeight = 0
    private var surfaceState = SurfaceState.UNINITIALIZED
    private var surface: Surface? = null
    private var windowSurface: EGLSurface? = null
    private var renderer: EmulatorRenderer? = null
    // LITEV_HYB_DIRECT_PRESENT: layout version handed to the GL present thread (-1: we draw)
    private var directVersion = -1

    private enum class SurfaceState {
        UNINITIALIZED,
        DIRTY,
        READY;
    }

    init {
        holder.addCallback(this)
    }

    fun setRenderer(emulatorRenderer: EmulatorRenderer) {
        renderer = emulatorRenderer
    }

    fun updateRendererConfiguration(newRendererConfiguration: RuntimeRendererConfiguration?) {
        renderer?.updateRendererConfiguration(newRendererConfiguration)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        synchronized(surfaceLock) {
            surface = holder.surface
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        synchronized(surfaceLock) {
            surfaceWidth = width
            surfaceHeight = height
            if (surfaceState == SurfaceState.READY) {
                surfaceState = SurfaceState.DIRTY
            }
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        synchronized(surfaceLock) {
            surface = null
        }
    }

    fun doFrame(glContext: GlContext, presentFrameWrapper: PresentFrameWrapper) {
        synchronized(surfaceLock) {
            if (windowSurface == null) {
                if (!setupWindowSurface(glContext)) {
                    return
                }
            } else if (surface == null) {
                // We had a surface, but it has been destroyed
                takeBackDirect()
                windowSurface?.let {
                    glContext.destroyWindowSurface(it)
                    windowSurface = null
                }
                return
            }

            // LITEV_HYB_DIRECT_PRESENT: when the frame needs nothing beyond the default screen
            // shader, the GL renderer's present thread draws it into this window as soon as it
            // is merged; this thread hands the surface over and stays out of it
            val layout = if (MelonEmulator.directPresentAvailable() && surfaceState == SurfaceState.READY && presentFrameWrapper.isValidFrame &&
                (directOwner == null || directOwner === this)) renderer?.directPresentLayout() else null
            if (layout != null) {
                if (directVersion != layout.version) {
                    glContext.releaseSurface()
                    MelonEmulator.setDirectPresent(windowSurface!!.nativeHandle, layout.vertices, layout.linear, layout.width, layout.height)
                    directVersion = layout.version
                    directOwner = this
                }
                return
            }
            takeBackDirect()

            PresentTimer.mark(6)
            glContext.use(windowSurface!!)
            PresentTimer.mark(7)
            GLES30.glViewport(0, 0, width, height)

            if (surfaceState == SurfaceState.UNINITIALIZED && renderer != null) {
                renderer?.onSurfaceCreated()
                surfaceState = SurfaceState.DIRTY
            }

            if (surfaceState == SurfaceState.DIRTY && renderer != null) {
                renderer?.onSurfaceChanged(surfaceWidth, surfaceHeight)
                surfaceState = SurfaceState.READY
            }

            renderer?.drawFrame(presentFrameWrapper)
            PresentTimer.mark(8)
            glContext.swapBuffers(windowSurface!!)
            PresentTimer.mark(9)
            PresentTimer.frame()
        }
    }

    private fun setupWindowSurface(glContext: GlContext): Boolean {
        val currentSurface = surface ?: return false
        windowSurface = glContext.createWindowSurface(currentSurface)
        return true
    }

    private fun takeBackDirect() {
        if (directVersion < 0) return
        MelonEmulator.setDirectPresent(0L, null, false, 0, 0)   // returns once the present thread let go
        directVersion = -1
        if (directOwner === this) directOwner = null
    }

    fun stop(glContext: GlContext) {
        synchronized(surfaceLock) {
            takeBackDirect()
            windowSurface?.let {
                glContext.destroyWindowSurface(it)
                windowSurface = null
            }
        }
    }

    private companion object {
        @Volatile var directOwner: EmulatorSurfaceView? = null   // one window at a time
    }
}
