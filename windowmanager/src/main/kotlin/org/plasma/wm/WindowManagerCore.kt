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

package org.plasma.wm

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlin.math.min

data class Frame(val x: Float, val y: Float, val w: Float, val h: Float) {
    val right get() = x + w
    val bottom get() = y + h
    fun leftHalf() = Frame(x, y, w / 2f, h)
    fun rightHalf() = Frame(x + w / 2f, y, w / 2f, h)
}

enum class WinState { NORMAL, MAXIMIZED, SNAP_LEFT, SNAP_RIGHT, FULLSCREEN }

object Edge { const val LEFT = 1; const val TOP = 2; const val RIGHT = 4; const val BOTTOM = 8 }

data class ManagedWindow(
    val id: Int,
    val title: String,
    val appId: String,
    val kind: String,
    val desktop: Int,
    val frame: Frame,
    val state: WinState = WinState.NORMAL,
    val restore: Frame? = null,
    val minimized: Boolean = false,
)

data class WmState(
    val windows: List<ManagedWindow>,
    val focusedId: Int?,
    val desktop: Int,
    val desktopCount: Int,
    val showingDesktop: Boolean,
    val workArea: Frame,
    val screenArea: Frame = workArea,
) {
    fun window(id: Int) = windows.firstOrNull { it.id == id }
    val visible: List<ManagedWindow>
        get() = if (showingDesktop) emptyList() else windows.filter { it.desktop == desktop && !it.minimized }
}

class WindowManagerCore(workArea: Frame, desktopCount: Int = 2) {
    companion object {
        const val MIN_W = 240f
        const val MIN_H = 150f
        const val SNAP_MARGIN = 24f
    }

    private val _state = MutableStateFlow(WmState(emptyList(), null, 0, desktopCount.coerceAtLeast(1), false, workArea))
    val state: StateFlow<WmState> = _state.asStateFlow()
    private var nextId = 1

    private fun topId(s: WmState): Int? =
        if (s.showingDesktop) null else s.windows.lastOrNull { it.desktop == s.desktop && !it.minimized }?.id

    private fun refocus(s: WmState) = s.copy(focusedId = topId(s))

    private fun fit(f: Frame, wa: Frame): Frame {
        val w = f.w.coerceIn(min(MIN_W, wa.w), wa.w)
        val h = f.h.coerceIn(min(MIN_H, wa.h), wa.h)
        return Frame(f.x.coerceIn(wa.x, wa.right - w), f.y.coerceIn(wa.y, wa.bottom - h), w, h)
    }

    private fun mapWin(id: Int, f: (ManagedWindow, WmState) -> ManagedWindow) = _state.update { s ->
        s.copy(windows = s.windows.map { if (it.id == id) f(it, s) else it })
    }

    fun setScreenArea(a: Frame) = _state.update { s ->
        s.copy(screenArea = a, windows = s.windows.map { w ->
            if (w.state == WinState.FULLSCREEN) w.copy(frame = a) else w
        })
    }

    fun setWorkArea(a: Frame) = _state.update { s ->
        s.copy(workArea = a, windows = s.windows.map { w ->
            when (w.state) {
                WinState.FULLSCREEN -> w.copy(frame = s.screenArea)
                WinState.MAXIMIZED -> w.copy(frame = a)
                WinState.SNAP_LEFT -> w.copy(frame = a.leftHalf())
                WinState.SNAP_RIGHT -> w.copy(frame = a.rightHalf())
                WinState.NORMAL -> w.copy(frame = fit(w.frame, a))
            }
        })
    }

    fun open(title: String, appId: String, kind: String, size: Frame? = null): Int {
        val id = nextId++
        _state.update { s ->
            val wa = s.workArea
            val n = s.windows.count { it.desktop == s.desktop }
            val off = (n % 8) * 36f
            val f = size ?: Frame(wa.x + 40f + off, wa.y + 30f + off, wa.w * 0.7f, wa.h * 0.7f)
            val win = ManagedWindow(id, title, appId, kind, s.desktop, fit(f, wa))
            s.copy(windows = s.windows + win, focusedId = id, showingDesktop = false)
        }
        return id
    }

    fun setTitle(id: Int, title: String) = mapWin(id) { w, _ -> w.copy(title = title) }

    fun focus(id: Int) = _state.update { s ->
        val w = s.window(id) ?: return@update s
        s.copy(
            windows = s.windows.filter { it.id != id } + w.copy(minimized = false),
            focusedId = id, desktop = w.desktop, showingDesktop = false,
        )
    }

    fun close(id: Int) = _state.update { s -> refocus(s.copy(windows = s.windows.filter { it.id != id })) }

    fun minimize(id: Int) = _state.update { s ->
        refocus(s.copy(windows = s.windows.map { if (it.id == id) it.copy(minimized = true) else it }))
    }

    fun toggleTask(id: Int) {
        val s = _state.value
        val w = s.window(id) ?: return
        if (w.minimized || s.focusedId != id || s.desktop != w.desktop || s.showingDesktop) focus(id) else minimize(id)
    }

    fun maximize(id: Int) = mapWin(id) { w, s ->
        if (w.state == WinState.MAXIMIZED) w
        else w.copy(restore = if (w.state == WinState.NORMAL) w.frame else w.restore, state = WinState.MAXIMIZED, frame = s.workArea)
    }

    fun restore(id: Int) = mapWin(id) { w, _ ->
        if (w.state == WinState.NORMAL) w else w.copy(state = WinState.NORMAL, frame = w.restore ?: w.frame, restore = null)
    }

    fun fullscreen(id: Int) = mapWin(id) { w, s ->
        if (w.state == WinState.FULLSCREEN) w
        else w.copy(restore = if (w.state == WinState.NORMAL) w.frame else w.restore, state = WinState.FULLSCREEN, frame = s.screenArea)
    }

    fun toggleFullscreen(id: Int) {
        val w = _state.value.window(id) ?: return
        if (w.state == WinState.FULLSCREEN) restore(id) else fullscreen(id)
    }

    fun rotate(id: Int) = _state.update { s ->
        val w = s.window(id) ?: return@update s
        if (w.state == WinState.FULLSCREEN) return@update s
        val base = if (w.state == WinState.NORMAL) w
        else w.copy(state = WinState.NORMAL, frame = w.restore ?: w.frame, restore = null)
        val turned = fit(WindowAspect.rotate(base.frame, s.workArea), s.workArea)
        s.copy(windows = s.windows.map { if (it.id == id) base.copy(frame = turned) else it })
    }

    fun toggleMaximize(id: Int) {
        val w = _state.value.window(id) ?: return
        if (w.state == WinState.MAXIMIZED) restore(id) else maximize(id)
    }

    fun snap(id: Int, left: Boolean) = mapWin(id) { w, s ->
        w.copy(
            restore = if (w.state == WinState.NORMAL) w.frame else w.restore,
            state = if (left) WinState.SNAP_LEFT else WinState.SNAP_RIGHT,
            frame = if (left) s.workArea.leftHalf() else s.workArea.rightHalf(),
        )
    }

    fun move(id: Int, dx: Float, dy: Float, pointerX: Float? = null) = mapWin(id) { w, s ->
        val wa = s.workArea
        if (w.state != WinState.NORMAL) {
            val r = w.restore ?: w.frame
            val cx = pointerX ?: (w.frame.x + w.frame.w / 2f)
            val nx = (cx - r.w / 2f).coerceIn(wa.x, (wa.right - r.w).coerceAtLeast(wa.x))
            w.copy(state = WinState.NORMAL, restore = null, frame = Frame(nx, wa.y, r.w, r.h))
        } else {
            val nx = (w.frame.x + dx).coerceIn(wa.x - w.frame.w + 80f, wa.right - 80f)
            val ny = (w.frame.y + dy).coerceIn(wa.y, wa.bottom - 40f)
            w.copy(frame = Frame(nx, ny, w.frame.w, w.frame.h))
        }
    }

    fun resize(id: Int, edges: Int, dx: Float, dy: Float) = mapWin(id) { w, _ ->
        if (w.state != WinState.NORMAL) return@mapWin w
        var x = w.frame.x; var y = w.frame.y; var ww = w.frame.w; var hh = w.frame.h
        if ((edges and Edge.LEFT) != 0) { val nw = (ww - dx).coerceAtLeast(MIN_W); x += ww - nw; ww = nw }
        if ((edges and Edge.RIGHT) != 0) ww = (ww + dx).coerceAtLeast(MIN_W)
        if ((edges and Edge.TOP) != 0) { val nh = (hh - dy).coerceAtLeast(MIN_H); y += hh - nh; hh = nh }
        if ((edges and Edge.BOTTOM) != 0) hh = (hh + dy).coerceAtLeast(MIN_H)
        w.copy(frame = Frame(x, y, ww, hh))
    }

    fun dragEnd(id: Int, px: Float, py: Float) {
        val wa = _state.value.workArea
        when {
            py <= wa.y + SNAP_MARGIN -> maximize(id)
            px <= wa.x + SNAP_MARGIN -> snap(id, true)
            px >= wa.right - SNAP_MARGIN -> snap(id, false)
        }
    }

    fun cycle(forward: Boolean) {
        val v = _state.value.visible
        if (v.size < 2) return
        focus(if (forward) v[v.size - 2].id else v.first().id)
    }

    fun toggleShowDesktop() = _state.update { s ->
        val on = !s.showingDesktop
        val n = s.copy(showingDesktop = on)
        if (on) n.copy(focusedId = null) else refocus(n)
    }

    fun switchDesktop(n: Int) = _state.update { s ->
        if (n !in 0 until s.desktopCount) s else refocus(s.copy(desktop = n, showingDesktop = false))
    }

    fun moveToDesktop(id: Int, n: Int) = _state.update { s ->
        if (n !in 0 until s.desktopCount) s
        else refocus(s.copy(windows = s.windows.map { if (it.id == id) it.copy(desktop = n) else it }))
    }

    fun setDesktopCount(n: Int) = _state.update { s ->
        val c = n.coerceAtLeast(1)
        refocus(s.copy(
            desktopCount = c, desktop = s.desktop.coerceAtMost(c - 1),
            windows = s.windows.map { if (it.desktop >= c) it.copy(desktop = c - 1) else it },
        ))
    }
}
