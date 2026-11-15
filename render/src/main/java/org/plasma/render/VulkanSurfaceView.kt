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

import android.content.Context
import android.view.SurfaceHolder
import android.view.SurfaceView

class VulkanSurfaceView(
    context: Context,
    private val compositor: VulkanCompositor,
) : SurfaceView(context), SurfaceHolder.Callback {
    init {
        setZOrderOnTop(false)
        setWillNotDraw(true)
        holder.addCallback(this)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        compositor.start(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        compositor.resize(width, height)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        compositor.stop()
    }
}
