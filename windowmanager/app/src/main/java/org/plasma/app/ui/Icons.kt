
package org.plasma.app.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

private class Ic(val stroke: String, val fill: String = "")

private val ICONS: Map<String, Ic> = mapOf(
    "star" to Ic("M8 1.8 L9.9 6 L14.4 6.5 L11 9.5 L12 14 L8 11.7 L4 14 L5 9.5 L1.6 6.5 L6.1 6 Z"),
    "palette" to Ic("M8 2 C4.5 2 2 4.6 2 8 C2 11.4 4.6 14 7.8 14 C9 14 9.6 13.2 9.1 12.2 C8.6 11.2 9.2 10.4 10.3 10.4 L12 10.4 C13.2 10.4 14 9.6 14 8.4 C14 4.8 11.4 2 8 2 Z M5 7.2 h.01 M8 5 h.01 M11 6.6 h.01"),
    "sparkle" to Ic("M8 1.5 L9.3 6.7 L14.5 8 L9.3 9.3 L8 14.5 L6.7 9.3 L1.5 8 L6.7 6.7 Z"),
    "panel" to Ic("M2 2.5 H14 V7.5 H2 Z M3.5 10 H12.5 A1.75 1.75 0 0 1 12.5 13.5 H3.5 A1.75 1.75 0 0 1 3.5 10 Z"),
    "terminal" to Ic("M2 3 H14 V13 H2 Z M4.5 6 L7 8 L4.5 10 M8.5 10.5 H11.5"),
    "monitor" to Ic("M2 3 H14 V10.5 H2 Z M6 13.5 H10 M8 10.5 V13.5"),
    "chip" to Ic("M4.5 4.5 H11.5 V11.5 H4.5 Z M6.5 6.5 H9.5 V9.5 H6.5 Z M6.5 2 V4.5 M9.5 2 V4.5 M6.5 11.5 V14 M9.5 11.5 V14 M2 6.5 H4.5 M2 9.5 H4.5 M11.5 6.5 H14 M11.5 9.5 H14"),
    "android" to Ic("M3.5 9.5 A4.5 4.5 0 0 1 12.5 9.5 Z M5 3 L6.2 5 M11 3 L9.8 5 M5.5 11.5 V14 M10.5 11.5 V14 M6.3 7.6 h.01 M9.7 7.6 h.01"),
    "share" to Ic("M2.5 5.5 H13 M10 2.8 L13 5.5 L10 8.2 M13.5 10.5 H3 M6 7.8 L3 10.5 L6 13.2"),
    "keyboard" to Ic("M1.5 4.5 H14.5 V11.5 H1.5 Z M4 7 h.01 M6.7 7 h.01 M9.3 7 h.01 M12 7 h.01 M5 9.5 H11"),
    "list" to Ic("M2.5 4 H13.5 M2.5 8 H13.5 M2.5 12 H13.5"),
    "info" to Ic("M8 14 A6 6 0 1 0 8 2 A6 6 0 0 0 8 14 Z M8 7.2 V11 M8 5 h.01"),
    "search" to Ic("M7 11.5 A4.5 4.5 0 1 0 7 2.5 A4.5 4.5 0 0 0 7 11.5 Z M10.4 10.4 L14 14"),
    "close" to Ic("M3.5 3.5 L12.5 12.5 M12.5 3.5 L3.5 12.5"),
    "plus" to Ic("M8 3 V13 M3 8 H13"),
    "check" to Ic("M3 8.5 L6.5 12 L13 4.5"),
    "chevL" to Ic("M10 3 L5 8 L10 13"),
    "chevR" to Ic("M6 3 L11 8 L6 13"),
    "arrowL" to Ic("M13 8 H3 M7.5 3.5 L3 8 L7.5 12.5"),
    "arrowUp" to Ic("M8 13 V3 M3.5 7.5 L8 3 L12.5 7.5"),
    "radioOff" to Ic("M8 14 A6 6 0 1 0 8 2 A6 6 0 0 0 8 14 Z"),
    "radioOn" to Ic("M8 14 A6 6 0 1 0 8 2 A6 6 0 0 0 8 14 Z", "M8 5.3 A2.7 2.7 0 1 0 8 10.7 A2.7 2.7 0 0 0 8 5.3 Z"),
    "checkOff" to Ic("M3.5 2.5 H12.5 A1 1 0 0 1 13.5 3.5 V12.5 A1 1 0 0 1 12.5 13.5 H3.5 A1 1 0 0 1 2.5 12.5 V3.5 A1 1 0 0 1 3.5 2.5 Z"),
    "checkOn" to Ic("M3.5 2.5 H12.5 A1 1 0 0 1 13.5 3.5 V12.5 A1 1 0 0 1 12.5 13.5 H3.5 A1 1 0 0 1 2.5 12.5 V3.5 A1 1 0 0 1 3.5 2.5 Z M5 8.2 L7.2 10.4 L11 5.8"),
    "folder" to Ic("M2 4.5 A1 1 0 0 1 3 3.5 H6.2 L7.8 5.2 H13 A1 1 0 0 1 14 6.2 V11.5 A1 1 0 0 1 13 12.5 H3 A1 1 0 0 1 2 11.5 Z"),
    "file" to Ic("M4 2 H9.5 L12.5 5 V13.5 A.5 .5 0 0 1 12 14 H4 A.5 .5 0 0 1 3.5 13.5 V2.5 A.5 .5 0 0 1 4 2 Z M9.5 2 V5 H12.5"),
    "bullet" to Ic("", "M8 5.5 A2.5 2.5 0 1 0 8 10.5 A2.5 2.5 0 0 0 8 5.5 Z"),
    "wifi" to Ic("M2 6 A8.5 8.5 0 0 1 14 6 M4 8.5 A5.5 5.5 0 0 1 12 8.5 M6 11 A2.5 2.5 0 0 1 10 11 M8 13.5 h.01"),
    "wifiOff" to Ic("M2 6 A8.5 8.5 0 0 1 14 6 M4 8.5 A5.5 5.5 0 0 1 12 8.5 M6 11 A2.5 2.5 0 0 1 10 11 M8 13.5 h.01 M2.5 2.5 L13.5 13.5"),
    "bluetooth" to Ic("M4.5 5 L11 11 L8 14 V2 L11 5 L4.5 11"),
    "volume" to Ic("M2 6 H4.5 L8 3 V13 L4.5 10 H2 Z M10.5 5.5 A3.5 3.5 0 0 1 10.5 10.5 M12.5 3.5 A6.5 6.5 0 0 1 12.5 12.5"),
    "volumeMute" to Ic("M2 6 H4.5 L8 3 V13 L4.5 10 H2 Z M10.5 6 L14 10 M14 6 L10.5 10"),
    "brightness" to Ic("M8 11 A3 3 0 1 0 8 5 A3 3 0 0 0 8 11 Z M8 1.5 V3 M8 13 V14.5 M1.5 8 H3 M13 8 H14.5 M3.4 3.4 L4.5 4.5 M11.5 11.5 L12.6 12.6 M3.4 12.6 L4.5 11.5 M11.5 4.5 L12.6 3.4"),
    "refresh" to Ic("M13 8 A5 5 0 1 1 11.5 4.5 M13 2.5 V5 H10.5"),
    "lock" to Ic("M4 7 H12 V13.5 H4 Z M5.5 7 V5 A2.5 2.5 0 0 1 10.5 5 V7"),
    "headphones" to Ic("M3 10 V8 A5 5 0 0 1 13 8 V10 M3 9.5 H5 V13 H3 Z M11 9.5 H13 V13 H11 Z"),
    "phone" to Ic("M5 2 H11 V14 H5 Z M7 12 H9"),
    "fullscreen" to Ic("M2 6 V2 H6 M10 2 H14 V6 M14 10 V14 H10 M6 14 H2 V10"),
    "fullscreenExit" to Ic("M6 2 V6 H2 M14 6 H10 V2 M10 14 V10 H14 M2 10 H6 V14"),

    "rotate" to Ic("M2.5 7 H9.5 V14 H2.5 Z M6 3.5 H10.5 A3 3 0 0 1 13.5 6.5 V8 M12 6.5 L13.5 8.2 L15 6.5"),
)

private val pathCache = HashMap<String, Path>()
private fun path(d: String): Path = pathCache.getOrPut(d) { PathParser().parsePathString(d).toPath() }

@Composable
fun BreezeIcon(name: String, size: Dp = 16.dp, tint: Color = LocalBreeze.current.text, modifier: Modifier = Modifier) {
    val ic = ICONS[name] ?: return
    Canvas(modifier.size(size)) {
        val s = this.size.width / 16f
        withTransform({ scale(s, s, Offset.Zero) }) {
            if (ic.stroke.isNotEmpty()) drawPath(path(ic.stroke), tint, style = Stroke(1.5f, cap = StrokeCap.Round, join = StrokeJoin.Round))
            if (ic.fill.isNotEmpty()) drawPath(path(ic.fill), tint)
        }
    }
}

@Composable
fun BulletText(text: String, size: androidx.compose.ui.unit.TextUnit = androidx.compose.ui.unit.TextUnit.Unspecified) {
    val b = LocalBreeze.current
    Row(Modifier, verticalAlignment = Alignment.Top) {
        BreezeIcon("bullet", 12.dp, b.accent, Modifier.padding(top = 2.dp))
        Spacer(Modifier.width(4.dp))
        T(text, if (size == androidx.compose.ui.unit.TextUnit.Unspecified) androidx.compose.ui.unit.TextUnit(12f, androidx.compose.ui.unit.TextUnitType.Sp) else size)
    }
}
