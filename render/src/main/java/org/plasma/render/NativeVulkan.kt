/*
 * This file belongs to the PlasmaDroid project.
 *
 * Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
 *
 * PlasmaDroid is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
 * or <https://www.gnu.org/licenses/gpl-3.0.html>.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 */

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
