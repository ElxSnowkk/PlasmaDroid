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

package org.plasma.core

import android.content.Context
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

object Keys {

    const val DEFAULT_UI_SCALE = 0.75f
    const val MIN_UI_SCALE = 0.5f
    const val MAX_UI_SCALE = 2f
    const val RENDER = "render.backend"
    const val DARK = "ui.dark"
    const val ACCENT = "ui.accent"
    const val PANEL_TOP = "ui.panelTop"
    const val PANEL_H = "ui.panelHeightDp"
    const val UI_SCALE = "ui.scale"
    const val DESKTOPS = "wm.desktops"
    const val WALLPAPER = "ui.wallpaper"
    const val PINNED = "ui.pinned"
    const val FAVS = "ui.favs"
    const val DESK_ICONS = "ui.deskIcons"
    const val WIDGETS = "ui.widgets"
    const val NOTES = "ui.notes"
    const val USER_NAME = "ui.userName"
    const val METHOD = "android.method"
    const val ANDROID_DPI = "android.dpi"
    const val ROOT_ON = "android.rootEnabled"
    const val FORCE_RESIZABLE = "android.forceResizable"

    const val FORCE_ASPECT = "wm.forceAspect"

    const val ASPECT_RATIO = "wm.aspectRatio"
    const val SHARE_W = "share.w"
    const val SHARE_H = "share.h"
    const val SHARE_FPS = "share.fps"
    const val SHARE_KBPS = "share.kbps"
    const val SHARE_CODEC = "share.codec"
    const val SHARE_PIN = "share.pin"
    const val SHARE_PORT = "share.port"
    const val SHARE_LOWLAT = "share.lowLatency"
    const val AUTO_EXTERNAL = "display.autoExternal"
    const val THEME = "ui.theme"
    const val FX_BLUR = "fx.blur"
    const val FX_BLUR_STRENGTH = "fx.blurStrength"
    const val FX_TRANSPARENCY = "fx.transparency"
    const val FX_SHADOWS = "fx.shadows"
    const val FX_ANIM_SPEED = "fx.animSpeed"
    const val FX_WALL_ANIM = "fx.wallpaperAnimated"
    const val FX_FPS = "fx.fpsCap"
    const val FX_SATURATION = "fx.saturation"
    const val FX_VIGNETTE = "fx.vignette"
    const val PANEL_FLOATING = "ui.panelFloating"
    const val PANEL_ADAPTIVE = "ui.panelAdaptive"

    const val PANEL_MARGIN = "ui.panelMarginDp"

    const val PANEL_RADIUS = "ui.panelRadiusDp"

    const val PANEL_WIDTH_PCT = "ui.panelWidthPct"
    const val SCALE_MODE = "ui.scaleConfirmed"
    const val TERM_FONT = "term.fontSp"
    const val TERM_OPACITY = "term.opacity"
    const val TERM_FASTFETCH = "term.fastfetch"
    const val TERM_SCHEME = "term.scheme"

    const val GFX_SERVER = "linux.gfxServer"
    const val VK_DRIVER = "vk.driver"

    const val GL_ANGLE = "gl.angle"

    const val VK_WSI_DMABUF = "vk.wsiDmabuf"
    const val VK_INJECTED = "vk.injectedName"
    const val VK_INJECTED_BIONIC = "vk.injectedBionicName"
    fun displayMode(id: Int) = "display.mode.$id"
    fun compat(pkg: String) = "compat.$pkg"
}

class SettingsStore(ctx: Context) {
    private val sp = ctx.getSharedPreferences("plasma", Context.MODE_PRIVATE)
    private val _version = MutableStateFlow(0)
    val version: StateFlow<Int> = _version.asStateFlow()

    fun string(k: String, d: String = ""): String = sp.getString(k, d) ?: d
    fun int(k: String, d: Int = 0): Int = sp.getInt(k, d)
    fun bool(k: String, d: Boolean = false): Boolean = sp.getBoolean(k, d)
    fun float(k: String, d: Float = 0f): Float = sp.getFloat(k, d)
    fun contains(k: String) = sp.contains(k)
    fun keysWithPrefix(p: String): List<String> = sp.all.keys.filter { it.startsWith(p) }

    private fun same(k: String, v: Any): Boolean {
        if (!sp.contains(k)) return false
        return try {
            when (v) {
                is String -> sp.getString(k, null) == v
                is Int -> sp.getInt(k, v + 1) == v
                is Boolean -> sp.getBoolean(k, !v) == v
                is Float -> sp.getFloat(k, v + 1f) == v
                is Long -> sp.getLong(k, v + 1L) == v
                else -> false
            }
        } catch (_: ClassCastException) { false }
    }

    private fun silent(k: String) = k.startsWith("compat.")

    fun put(k: String, v: Any) {
        if (same(k, v)) return
        sp.edit().apply {
            when (v) {
                is String -> putString(k, v)
                is Int -> putInt(k, v)
                is Boolean -> putBoolean(k, v)
                is Float -> putFloat(k, v)
                is Long -> putLong(k, v)
                else -> error("tipo nao suportado: ${v.javaClass}")
            }
        }.apply()
        if (!silent(k)) _version.value++
    }

    fun remove(k: String) {
        if (!sp.contains(k)) return
        sp.edit().remove(k).apply()
        _version.value++
    }

    fun list(k: String, d: String = ""): List<String> = string(k, d).split(',').filter { it.isNotBlank() }
    fun putList(k: String, l: List<String>) = put(k, l.joinToString(","))
}
