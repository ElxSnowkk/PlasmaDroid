
package org.plasma.display

import android.app.ActivityOptions
import android.content.Context
import android.content.Intent
import android.hardware.display.DisplayManager
import android.os.Handler
import android.os.Looper
import android.view.Display
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.plasma.core.PlasmaLog

data class DisplayModeInfo(val id: Int, val width: Int, val height: Int, val fps: Float) {
    val label get() = "${width}×$height @ ${"%.0f".format(fps)} Hz"
}

data class DisplayEntry(
    val id: Int,
    val name: String,
    val isDefault: Boolean,
    val isExternal: Boolean,
    val width: Int,
    val height: Int,
    val fps: Float,
    val densityDpi: Int,
    val currentModeId: Int,
    val modes: List<DisplayModeInfo>,
)

class DisplayOutputManager(private val ctx: Context) {
    private val dm = ctx.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
    private val _displays = MutableStateFlow<List<DisplayEntry>>(emptyList())
    val displays: StateFlow<List<DisplayEntry>> = _displays.asStateFlow()
    private var started = false

    private val listener = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(displayId: Int) { PlasmaLog.i("display", "display adicionado: $displayId"); refresh() }
        override fun onDisplayRemoved(displayId: Int) { PlasmaLog.i("display", "display removido: $displayId"); refresh() }
        override fun onDisplayChanged(displayId: Int) { refresh() }
    }

    fun start() {
        if (started) return
        started = true
        dm.registerDisplayListener(listener, Handler(Looper.getMainLooper()))
        refresh()
    }

    fun refresh() {
        val presentation = dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION).map { it.displayId }.toSet()
        _displays.value = dm.displays.filter { it.state != Display.STATE_UNKNOWN || it.displayId == Display.DEFAULT_DISPLAY }.map { d ->
            val mode = d.mode
            DisplayEntry(
                id = d.displayId,
                name = d.name ?: "Display ${d.displayId}",
                isDefault = d.displayId == Display.DEFAULT_DISPLAY,
                isExternal = d.displayId != Display.DEFAULT_DISPLAY && d.displayId in presentation,
                width = mode.physicalWidth, height = mode.physicalHeight, fps = d.refreshRate,
                densityDpi = ctx.resources.displayMetrics.densityDpi,
                currentModeId = mode.modeId,
                modes = d.supportedModes.map { DisplayModeInfo(it.modeId, it.physicalWidth, it.physicalHeight, it.refreshRate) }
                    .sortedWith(compareByDescending<DisplayModeInfo> { it.width * it.height }.thenByDescending { it.fps }),
            )
        }
    }

    fun externals(): List<DisplayEntry> = _displays.value.filter { it.isExternal }

    fun launchOn(intent: Intent, displayId: Int): String? = try {
        val opts = ActivityOptions.makeBasic().setLaunchDisplayId(displayId)
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_MULTIPLE_TASK)
        ctx.startActivity(intent, opts.toBundle())
        PlasmaLog.i("display", "desktop aberto no display $displayId")
        null
    } catch (t: Throwable) {
        PlasmaLog.e("display", "falha ao abrir no display $displayId", t); t.message ?: t.toString()
    }
}
