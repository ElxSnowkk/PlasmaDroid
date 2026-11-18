
package org.plasma.render

import android.view.Surface

internal object NativeVulkan {
    init { System.loadLibrary("plasmarender") }

    external fun hasCompositor(): Boolean
    external fun create(surface: Surface): Long
    external fun configure(
        handle: Long,
        style: Int,
        paletteRgb: FloatArray,
        animated: Boolean,
        fpsCap: Int,
        blur: Boolean,
        blurStrength: Float,
        vignette: Float,
        dim: Float,
    )
    external fun setScene(handle: Long, scene: FloatArray)
    external fun resize(handle: Long, width: Int, height: Int)
    external fun deviceName(handle: Long): String
    external fun isRunning(handle: Long): Boolean
    external fun destroy(handle: Long)
}
