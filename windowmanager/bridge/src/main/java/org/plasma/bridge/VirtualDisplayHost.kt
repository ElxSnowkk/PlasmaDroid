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

package org.plasma.bridge

import android.content.Context
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.view.Surface
import org.plasma.core.PlasmaLog

class VirtualDisplayHost(private val ctx: Context) {
    private val dm = ctx.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager

    fun create(name: String, surface: Surface, w: Int, h: Int, dpi: Int): VirtualDisplay? {
        val flags = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC or DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY or
            DisplayFlags.SUPPORTS_TOUCH or DisplayFlags.DESTROY_CONTENT_ON_REMOVAL
        return try {
            dm.createVirtualDisplay(name, w, h, dpi, surface, flags)
        } catch (t: Throwable) {
            PlasmaLog.w("vdisplay", "createVirtualDisplay (app) falhou; tentando flags básicas", t)
            try {
                dm.createVirtualDisplay(name, w, h, dpi, surface, DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY)
            } catch (t2: Throwable) { PlasmaLog.e("vdisplay", "falha definitiva", t2); null }
        }
    }
}
