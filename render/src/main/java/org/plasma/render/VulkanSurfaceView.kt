
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
