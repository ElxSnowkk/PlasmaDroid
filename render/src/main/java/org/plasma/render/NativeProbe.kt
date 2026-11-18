
package org.plasma.render

object NativeProbe {
    init { System.loadLibrary("plasmarender") }
    @JvmStatic external fun probeVulkan(selfTest: Boolean): String
    @JvmStatic external fun probeGles(selfTest: Boolean): String
    @JvmStatic external fun hasVulkanCompositor(): Boolean
}
