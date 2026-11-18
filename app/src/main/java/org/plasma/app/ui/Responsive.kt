
package org.plasma.app.ui

import android.content.Context
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import org.plasma.core.Keys
import org.plasma.core.SettingsStore
import kotlin.math.roundToInt

object UiScale {

    fun auto(ctx: Context): Float {
        val m = ctx.resources.displayMetrics
        val shortDp = minOf(m.widthPixels, m.heightPixels) / m.density
        return ((shortDp / 400f).coerceIn(0.9f, 1.2f) * 20f).roundToInt() / 20f
    }

    fun isAuto(st: SettingsStore): Boolean = st.bool(Keys.UI_SCALE_AUTO, !st.contains(Keys.UI_SCALE))

    fun effective(ctx: Context, st: SettingsStore): Float =
        if (isAuto(st)) auto(ctx) else st.float(Keys.UI_SCALE, Keys.DEFAULT_UI_SCALE).coerceIn(Keys.MIN_UI_SCALE, Keys.MAX_UI_SCALE)

    fun androidDpi(ctx: Context, st: SettingsStore): Int {
        val v = st.int(Keys.ANDROID_DPI, 0)
        if (v > 0) return v
        val d = ctx.resources.displayMetrics.densityDpi
        return ((d * 0.55f / 20f).roundToInt() * 20).coerceIn(160, 320)
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ButtonRow(modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    FlowRow(
        modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) { content() }
}
