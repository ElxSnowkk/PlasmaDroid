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

package org.plasma.app.ui

import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.animation.core.Easing
import androidx.compose.animation.core.TweenSpec
import androidx.compose.animation.core.tween
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.remember
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import org.plasma.core.Keys
import org.plasma.core.SettingsStore
import kotlin.math.roundToInt

@Immutable
data class Breeze(
    val dark: Boolean, val window: Color, val view: Color, val text: Color, val dim: Color,
    val accent: Color, val panel: Color, val titleOn: Color, val titleOff: Color, val line: Color, val hover: Color,
    val titleTextOn: Color, val titleTextOff: Color, val button: Color,
    val positive: Color = Color(0xFF27AE60), val negative: Color = Color(0xFFDA4453), val neutral: Color = Color(0xFFF67400),
) {
    val onAccent: Color get() = Color(0xFFFCFCFC)
}

@Immutable
data class GlobalTheme(
    val id: String, val label: String, val author: String, val dark: Boolean, val accent: Long,
    val window: Long, val view: Long, val button: Long, val text: Long, val dim: Long,
    val titleOn: Long, val titleOff: Long, val line: Long, val wallpaper: String,
)

object Themes {
    val ALL = listOf(
        GlobalTheme("breeze-dark", "Breeze Dark", "KDE Visual Design Group", true, 0xFF3DAEE9, 0xFF202326, 0xFF141618, 0xFF292C30,
            0xFFFCFCFC, 0xFFA1A9B1, 0xFF272C31, 0xFF202428, 0xFF3B4045, "next"),
        GlobalTheme("breeze-light", "Breeze", "KDE Visual Design Group", false, 0xFF3DAEE9, 0xFFEFF0F1, 0xFFFFFFFF, 0xFFFCFCFC,
            0xFF232629, 0xFF707D8A, 0xFFE3E5E7, 0xFFEFF0F1, 0xFFBDC3C7, "light"),
        GlobalTheme("nord", "Nord", "Arctic Ice Studio", true, 0xFF88C0D0, 0xFF2E3440, 0xFF272C36, 0xFF3B4252,
            0xFFECEFF4, 0xFF9AA5B8, 0xFF3B4252, 0xFF2E3440, 0xFF4C566A, "nord"),
        GlobalTheme("mocha", "Catppuccin Mocha", "Catppuccin", true, 0xFFCBA6F7, 0xFF1E1E2E, 0xFF181825, 0xFF313244,
            0xFFCDD6F4, 0xFFA6ADC8, 0xFF313244, 0xFF1E1E2E, 0xFF45475A, "mocha"),
        GlobalTheme("dracula", "Dracula", "Zeno Rocha", true, 0xFFBD93F9, 0xFF282A36, 0xFF21222C, 0xFF343746,
            0xFFF8F8F2, 0xFFB6B9CC, 0xFF343746, 0xFF282A36, 0xFF44475A, "dracula"),
        GlobalTheme("gruvbox", "Gruvbox", "Pavel Pertsev", true, 0xFFFE8019, 0xFF282828, 0xFF1D2021, 0xFF3C3836,
            0xFFEBDBB2, 0xFFA89984, 0xFF3C3836, 0xFF282828, 0xFF504945, "gruvbox"),
        GlobalTheme("aurora", "Breeze Aurora", "Plasma Android", true, 0xFF26D98C, 0xFF1B2B2B, 0xFF121E1E, 0xFF223636,
            0xFFE6F2EE, 0xFF9DB5AE, 0xFF223636, 0xFF1B2B2B, 0xFF365050, "aurora"),
        GlobalTheme("sunset", "Pôr do Sol", "Plasma Android", true, 0xFFF29B4B, 0xFF2A1F3A, 0xFF1F162B, 0xFF382A4C,
            0xFFF5EAF0, 0xFFC2AFC4, 0xFF382A4C, 0xFF2A1F3A, 0xFF4F3B66, "sunset"),
    )
    fun byId(id: String): GlobalTheme = ALL.firstOrNull { it.id == id } ?: ALL[0]
}

val ACCENTS = listOf(0xFF3DAEE9, 0xFF1ABC9C, 0xFF27AE60, 0xFFFDBC4B, 0xFFF67400, 0xFFDA4453, 0xFFED1515, 0xFF9B59B6, 0xFFE91E63, 0xFF7F8C8D)

fun breeze(t: GlobalTheme, accent: Color): Breeze = Breeze(
    dark = t.dark, window = Color(t.window), view = Color(t.view), text = Color(t.text), dim = Color(t.dim), accent = accent,
    panel = Color(t.window), titleOn = Color(t.titleOn), titleOff = Color(t.titleOff), line = Color(t.line),
    hover = if (t.dark) Color(0x1FFFFFFF) else Color(0x14000000),
    titleTextOn = Color(t.text), titleTextOff = Color(t.dim), button = Color(t.button),
)

val LocalBreeze = staticCompositionLocalOf { breeze(Themes.ALL[0], Color(0xFF3DAEE9)) }

object U {
    val grid = 18.dp
    val iconSm = 16.dp; val iconSmM = 22.dp; val iconM = 32.dp; val iconL = 48.dp
    const val DEFAULT_PANEL_DP = 46
    const val DEFAULT_ACCENT = 0xFF3DAEE9.toInt()
    const val THEME_ACCENT = 0
    const val DEFAULT_PANEL_MARGIN_DP = 12
    const val DEFAULT_PANEL_RADIUS_DP = 14
    const val DEFAULT_PANEL_WIDTH_PCT = 100
    val frameRadius = 5.dp
    val spacing = 6.dp; val topMargin = 10.dp; val menuItemMargin = 4.dp
}

@Immutable
data class Fx(
    val blur: Boolean = true,
    val transparency: Float = 0.5f,
    val shadows: Boolean = true,
    val animSpeed: Float = 1f,
    val gpu: Boolean = true,
) {
    val realBlur: Boolean get() = blur && gpu
    fun alpha(kind: Float = 1f): Float {
        val range = if (realBlur) 0.55f else 0.30f
        val a = 1f - range * transparency * (2f - kind).coerceIn(0.3f, 1.7f)
        return a.coerceIn(0.25f, 1f)
    }
    fun ms(base: Int): Int = (base * animSpeed).roundToInt()
}

val LocalFx = staticCompositionLocalOf { Fx() }

object Motion {
    val OutCubic: Easing = CubicBezierEasing(0.215f, 0.61f, 0.355f, 1f)
    val InOutCubic: Easing = CubicBezierEasing(0.645f, 0.045f, 0.355f, 1f)
    val InCubic: Easing = CubicBezierEasing(0.55f, 0.055f, 0.675f, 0.19f)
    const val VERY_SHORT = 50
    const val SHORT = 100
    const val LONG = 200
    const val VERY_LONG = 400
    fun <T> spec(fx: Fx, ms: Int, easing: Easing = OutCubic): TweenSpec<T> = tween(fx.ms(ms), easing = easing)
}

@Composable
fun PlasmaTheme(settings: SettingsStore, gpuAlive: Boolean = true, content: @Composable () -> Unit) {
    settings.watch()
    val theme = Themes.byId(settings.string(Keys.THEME, "breeze-dark"))
    val accentRaw = settings.int(Keys.ACCENT, U.THEME_ACCENT)
    val accent = if (accentRaw == U.THEME_ACCENT) Color(theme.accent) else Color(accentRaw)
    val b = remember(theme, accent) { breeze(theme, accent) }
    val fx = Fx(
        blur = settings.bool(Keys.FX_BLUR, true),
        transparency = settings.float(Keys.FX_TRANSPARENCY, 0.5f),
        shadows = settings.bool(Keys.FX_SHADOWS, true),
        animSpeed = settings.float(Keys.FX_ANIM_SPEED, 1f),
        gpu = gpuAlive,
    )
    val scale = settings.float(Keys.UI_SCALE, Keys.DEFAULT_UI_SCALE).coerceIn(Keys.MIN_UI_SCALE, Keys.MAX_UI_SCALE)
    val d = LocalDensity.current
    val scaled = remember(d.density, d.fontScale, scale) { Density(d.density * scale, d.fontScale) }
    val scheme = if (theme.dark) darkColorScheme(primary = accent, background = b.window, surface = b.window, onSurface = b.text,
        onBackground = b.text, surfaceVariant = b.view, onSurfaceVariant = b.dim)
    else lightColorScheme(primary = accent, background = b.window, surface = b.window, onSurface = b.text,
        onBackground = b.text, surfaceVariant = b.view, onSurfaceVariant = b.dim)
    CompositionLocalProvider(LocalBreeze provides b, LocalDensity provides scaled, LocalFx provides fx) {
        MaterialTheme(colorScheme = scheme) { content() }
    }
}
