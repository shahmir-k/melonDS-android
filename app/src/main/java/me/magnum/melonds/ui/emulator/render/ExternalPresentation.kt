package me.magnum.melonds.ui.emulator.render

import android.app.Presentation
import android.content.Context
import android.graphics.Color
import android.os.Build
import android.view.Display
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.isVisible
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ViewModelStoreOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.lifecycle.setViewTreeViewModelStoreOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import me.magnum.melonds.domain.model.RuntimeBackground
import me.magnum.melonds.domain.model.layout.LayoutComponent
import me.magnum.melonds.ui.emulator.DSRenderer
import me.magnum.melonds.ui.emulator.EmulatorSurfaceView
import me.magnum.melonds.ui.emulator.RuntimeLayoutView
import me.magnum.melonds.ui.emulator.model.RuntimeInputLayoutConfiguration
import me.magnum.melonds.ui.emulator.model.RuntimeRendererConfiguration
import me.magnum.melonds.ui.layouteditor.model.LayoutTarget
import kotlin.collections.orEmpty

class ExternalPresentation(
    context: Context,
    display: Display,
    private val frameRenderCoordinator: FrameRenderCoordinator,
) : Presentation(context, display) {

    val layoutView = RuntimeLayoutView(context)
    private val container = FrameLayout(context)
    private val pauseOverlay = View(context)
    private val emulatorRenderer: DSRenderer
    private val surfaceView: EmulatorSurfaceView
    private var currentBackground: RuntimeBackground? = null
    private var currentRendererConfiguration: RuntimeRendererConfiguration? = null

    init {
        // Focusable, because only the focused window on a display can hide that display's system
        // bars: on a dual-screen handheld (AYN Thor) the bottom screen has its own navigation bar,
        // which otherwise covers the DS bottom screen for good. Touching this screen can then move
        // input focus here, so controller events are forwarded to the activity (see below).
        window?.setFlags(WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL, WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL)
        setCancelable(false)

        val layoutChangeListener = View.OnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            updateRendererScreenAreas()
        }

        container.addOnLayoutChangeListener(layoutChangeListener)
        emulatorRenderer = DSRenderer(context).also {
            surfaceView = createSurfaceView(it)
        }
        surfaceView.updateRendererConfiguration(currentRendererConfiguration)

        container.addView(surfaceView)
        container.addView(layoutView)
        container.addView(pauseOverlay)

        pauseOverlay.apply {
            setBackgroundColor(Color.BLACK)
            alpha = 0.6f
            isVisible = false
            setOnClickListener {
                // Do nothing. Just intercept clicks
            }
        }

        frameRenderCoordinator.addSurface(surfaceView)

        (context as? LifecycleOwner)?.let { owner ->
            container.setViewTreeLifecycleOwner(owner)
        }
        (context as? ViewModelStoreOwner)?.let { owner ->
            container.setViewTreeViewModelStoreOwner(owner)
        }
        (context as? SavedStateRegistryOwner)?.let { owner ->
            container.setViewTreeSavedStateRegistryOwner(owner)
        }

        setContentView(container)
    }

    fun swapScreens() {
        layoutView.swapScreens()
        updateRendererScreenAreas()
    }

    fun updateRendererScreenAreas() {
        val (topScreen, bottomScreen) = if (layoutView.areScreensSwapped()) {
            LayoutComponent.BOTTOM_SCREEN to LayoutComponent.TOP_SCREEN
        } else {
            LayoutComponent.TOP_SCREEN to LayoutComponent.BOTTOM_SCREEN
        }
        val topView = layoutView.getLayoutComponentView(topScreen)
        val bottomView = layoutView.getLayoutComponentView(bottomScreen)
        emulatorRenderer.updateScreenAreas(
            topScreenRect = topView?.getRect(),
            bottomScreenRect = bottomView?.getRect(),
            topAlpha = topView?.baseAlpha ?: 1f,
            bottomAlpha = bottomView?.baseAlpha ?: 1f,
            topOnTop = topView?.onTop ?: false,
            bottomOnTop = bottomView?.onTop ?: false,
        )

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val touchScreenArea = bottomView?.getRect()?.let {
                val rect = android.graphics.Rect(it.x, it.y, it.right, it.bottom)
                listOf(rect)
            }
            layoutView.systemGestureExclusionRects = touchScreenArea.orEmpty()
        }
    }

    fun setPauseOverlayVisibility(visible: Boolean) {
        pauseOverlay.isVisible = visible
    }

    private fun createSurfaceView(renderer: EmulatorRenderer): EmulatorSurfaceView {
        return EmulatorSurfaceView(context).apply {
            setRenderer(renderer)
            isFocusable = false
            isFocusableInTouchMode = false
        }
    }

    fun updateLayout(layoutConfiguration: RuntimeInputLayoutConfiguration) {
        layoutView.instantiateLayout(layoutConfiguration, LayoutTarget.SECONDARY_SCREEN)
        updateRendererScreenAreas()
    }

    fun updateRendererConfiguration(newRendererConfiguration: RuntimeRendererConfiguration?) {
        currentRendererConfiguration = newRendererConfiguration
        surfaceView.updateRendererConfiguration(newRendererConfiguration)
    }

    override fun onStart() {
        super.onStart()
        hideSystemBars()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    private fun hideSystemBars() {
        val window = window ?: return
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowCompat.getInsetsController(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    // Controller input belongs to the emulator, whichever screen has focus.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        return ownerActivity?.dispatchKeyEvent(event) ?: super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        return ownerActivity?.dispatchGenericMotionEvent(event) ?: super.dispatchGenericMotionEvent(event)
    }

    override fun onStop() {
        super.onStop()
        frameRenderCoordinator.removeSurface(surfaceView)
    }

    fun updateBackground(background: RuntimeBackground) {
        currentBackground = background
        emulatorRenderer.setBackground(background)
    }
}