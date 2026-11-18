
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
