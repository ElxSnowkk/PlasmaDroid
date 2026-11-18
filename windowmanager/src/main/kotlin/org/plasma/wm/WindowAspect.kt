
package org.plasma.wm

import kotlin.math.max
import kotlin.math.min

enum class Orientation {
    LANDSCAPE, PORTRAIT;

    val flipped: Orientation get() = if (this == LANDSCAPE) PORTRAIT else LANDSCAPE
}

object WindowAspect {

    const val SCREEN = "screen"
    const val DEFAULT = "16:9"
    val PRESETS: List<String> = listOf("16:9", "16:10", "4:3", "21:9", SCREEN)

    fun orientationOf(area: Frame): Orientation =
        if (area.w >= area.h) Orientation.LANDSCAPE else Orientation.PORTRAIT

    fun parse(preset: String): Float? {
        val p = preset.split(':')
        if (p.size != 2) return null
        val a = p[0].toFloatOrNull() ?: return null
        val b = p[1].toFloatOrNull() ?: return null
        if (a <= 0f || b <= 0f) return null
        return max(a, b) / min(a, b)
    }

    fun targetRatio(preset: String, screen: Frame, orientation: Orientation): Float {
        val long = parse(preset)
            ?: (max(screen.w, screen.h) / min(screen.w, screen.h).coerceAtLeast(1f))
        return if (orientation == Orientation.LANDSCAPE) long else 1f / long
    }

    fun fit(area: Frame, ratio: Float): Frame {
        if (area.w <= 0f || area.h <= 0f || ratio <= 0f) return area
        val byWidthH = area.w / ratio
        val w: Float
        val h: Float
        if (byWidthH <= area.h) { w = area.w; h = byWidthH } else { w = area.h * ratio; h = area.h }

        val fw = minOf((w + 0.001f).toInt().toFloat(), area.w).coerceAtLeast(1f)
        val fh = minOf((h + 0.001f).toInt().toFloat(), area.h).coerceAtLeast(1f)
        return Frame(area.x + (area.w - fw) / 2f, area.y + (area.h - fh) / 2f, fw, fh)
    }

    fun rotate(frame: Frame, workArea: Frame): Frame {
        val cx = frame.x + frame.w / 2f
        val cy = frame.y + frame.h / 2f
        var w = frame.h
        var h = frame.w
        val k = min(1f, min(workArea.w / w, workArea.h / h))
        w *= k; h *= k
        return Frame(cx - w / 2f, cy - h / 2f, w, h)
    }
}
