package me.magnum.melonds.impl.input

import me.magnum.melonds.domain.model.ControllerConfiguration

interface ControllerConfigurationFactory {
    fun buildDefaultControllerConfiguration(): ControllerConfiguration

    /** True if a saved configuration is an outdated default the user never changed. */
    fun isReplaceableDefault(configuration: ControllerConfiguration): Boolean = false
}