package me.magnum.melonds.impl.input

import android.os.Build
import android.view.KeyEvent
import android.view.MotionEvent
import me.magnum.melonds.domain.model.ControllerConfiguration
import me.magnum.melonds.domain.model.Input
import me.magnum.melonds.domain.model.InputConfig

/**
 * Default button mapping. Android names face buttons by Xbox position (BUTTON_A = bottom), so for
 * an ordinary controller the DS buttons are mapped by position: DS A (right) = BUTTON_B, and so on.
 * Handhelds with Nintendo-labelled buttons instead send codes by label (the A button, on the right,
 * sends BUTTON_A), so on those the DS buttons are mapped by label, A to A. Both report the D-pad as
 * the hat axis (the Thor's key layout also lists D-pad keys, but its controller sends HAT_X/HAT_Y).
 */
class DefaultControllerConfigurationFactory : ControllerConfigurationFactory {

    private val isNintendoLabelledHandheld =
        (Build.MANUFACTURER == "Anbernic" && Build.MODEL == "RG DS") || (Build.MANUFACTURER == "AYN" && Build.MODEL == "AYN Thor")

    override fun buildDefaultControllerConfiguration(): ControllerConfiguration {
        return if (isNintendoLabelledHandheld) buildConfiguration(byLabel = true, dpadKeys = false) else buildPositional()
    }

    override fun isReplaceableDefault(configuration: ControllerConfiguration): Boolean {
        // Saved from an earlier default and never changed: any earlier default without fast-forward on
        // R2; on the labelled handhelds also the position-based one, or the short-lived Thor default
        // that mapped D-pad keys instead of the hat axis (its D-pad then did nothing).
        val current = buildDefaultControllerConfiguration().inputMapper
        if (configuration.inputMapper == current) return false
        val earlier = mutableListOf(buildConfiguration(byLabel = isNintendoLabelledHandheld, dpadKeys = false, fastForward = false))
        if (isNintendoLabelledHandheld) {
            earlier += buildConfiguration(byLabel = false, dpadKeys = false, fastForward = false)
            earlier += buildConfiguration(byLabel = false, dpadKeys = false)
            earlier += buildConfiguration(byLabel = true, dpadKeys = true, fastForward = false)
            earlier += buildConfiguration(byLabel = true, dpadKeys = true)
        }
        return earlier.any { configuration.inputMapper == it.inputMapper }
    }

    private fun buildPositional() = buildConfiguration(byLabel = false, dpadKeys = false)

    private fun buildConfiguration(byLabel: Boolean, dpadKeys: Boolean, fastForward: Boolean = true): ControllerConfiguration {
        fun key(code: Int) = InputConfig.Assignment.Key(null, code)
        fun axis(code: Int, direction: InputConfig.Assignment.Axis.Direction) = InputConfig.Assignment.Axis(null, code, direction)
        val neg = InputConfig.Assignment.Axis.Direction.NEGATIVE
        val pos = InputConfig.Assignment.Axis.Direction.POSITIVE
        // D-pad first, left stick as the alternative
        fun direction(input: Input, dpadKey: Int, hatAxis: Int, stickAxis: Int, dir: InputConfig.Assignment.Axis.Direction) =
            InputConfig(input, if (dpadKeys) key(dpadKey) else axis(hatAxis, dir), axis(stickAxis, dir))

        val inputList = listOf(
            InputConfig(Input.A, key(if (byLabel) KeyEvent.KEYCODE_BUTTON_A else KeyEvent.KEYCODE_BUTTON_B)),
            InputConfig(Input.B, key(if (byLabel) KeyEvent.KEYCODE_BUTTON_B else KeyEvent.KEYCODE_BUTTON_A)),
            InputConfig(Input.X, key(if (byLabel) KeyEvent.KEYCODE_BUTTON_X else KeyEvent.KEYCODE_BUTTON_Y)),
            InputConfig(Input.Y, key(if (byLabel) KeyEvent.KEYCODE_BUTTON_Y else KeyEvent.KEYCODE_BUTTON_X)),
            direction(Input.LEFT, KeyEvent.KEYCODE_DPAD_LEFT, MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_X, neg),
            direction(Input.RIGHT, KeyEvent.KEYCODE_DPAD_RIGHT, MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_X, pos),
            direction(Input.UP, KeyEvent.KEYCODE_DPAD_UP, MotionEvent.AXIS_HAT_Y, MotionEvent.AXIS_Y, neg),
            direction(Input.DOWN, KeyEvent.KEYCODE_DPAD_DOWN, MotionEvent.AXIS_HAT_Y, MotionEvent.AXIS_Y, pos),
            InputConfig(Input.L, key(KeyEvent.KEYCODE_BUTTON_L1)),
            InputConfig(Input.R, key(KeyEvent.KEYCODE_BUTTON_R1)),
            InputConfig(Input.START, key(KeyEvent.KEYCODE_BUTTON_START)),
            InputConfig(Input.SELECT, key(KeyEvent.KEYCODE_BUTTON_SELECT)),
            InputConfig(Input.PAUSE, key(KeyEvent.KEYCODE_BUTTON_MODE)),
        ) + listOfNotNull(
            // R2 as a key, or the analog trigger on controllers that report it only as an axis
            // (a press that arrives as both toggles once: see onFastForwardPressed)
            InputConfig(Input.FAST_FORWARD, key(KeyEvent.KEYCODE_BUTTON_R2), axis(MotionEvent.AXIS_RTRIGGER, pos)).takeIf { fastForward },
        )

        return ControllerConfiguration(inputList)
    }
}
