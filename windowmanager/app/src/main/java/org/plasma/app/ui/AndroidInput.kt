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

import android.view.KeyEvent
import android.view.MotionEvent
import androidx.compose.ui.ExperimentalComposeUiApi
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.*
import org.plasma.app.android.InputPump
import org.plasma.bridge.PrivilegedBackend
import org.plasma.core.PlasmaLog
import org.plasma.app.session.WindowContent

@OptIn(ExperimentalComposeUiApi::class)
internal fun Modifier.androidInput(
    c: WindowContent.AndroidWin,
    currentBackend: () -> PrivilegedBackend?,
): Modifier = pointerInput(c) {
    awaitPointerEventScope {
        val pressed = ArrayList<PointerId>()
        val androidId = HashMap<PointerId, Int>()
        var downTime = 0L
        var lastTime = 0L
        var swallow = false
        var warnedDrop = false

        fun reset() { pressed.clear(); androidId.clear(); swallow = false }
        fun freeId(): Int { var i = 0; while (androidId.containsValue(i)) i++; return i }

        while (true) {
            val ev = awaitPointerEvent(PointerEventPass.Initial)

            val be = currentBackend() ?: c.backend
            val display = c.displayId
            if (be == null || display <= 0) {
                if (!warnedDrop && ev.changes.any { it.changedToDownIgnoreConsumed() }) {
                    warnedDrop = true
                    PlasmaLog.w("input", "toque descartado: backend=${be != null} display=$display (${c.app.pkg})")
                }
                if (pressed.isNotEmpty()) reset()
                continue
            }
            warnedDrop = false

            val first = ev.changes.firstOrNull() ?: continue
            val mouse = first.type == PointerType.Mouse
            val buttons = if (mouse && ev.buttons.isPrimaryPressed) MotionEvent.BUTTON_PRIMARY else 0

            if (ev.type == PointerEventType.Scroll) {
                val x = first.position.x; val y = first.position.y
                val sv = -first.scrollDelta.y; val sh = -first.scrollDelta.x
                val t = first.uptimeMillis
                InputPump.post { be.scroll(display, x, y, sv, sh, t) }
                ev.changes.forEach { it.consume() }
                continue
            }

            if (ev.type == PointerEventType.Exit && pressed.isNotEmpty() && mouse) {
                val t = maxOf(first.uptimeMillis, lastTime)
                val dt = downTime
                val ids = IntArray(pressed.size) { androidId[pressed[it]] ?: it }
                val xy = FloatArray(pressed.size * 2)
                pressed.forEachIndexed { i, pid ->
                    val ch = ev.changes.firstOrNull { it.id == pid } ?: first
                    xy[i * 2] = ch.position.x; xy[i * 2 + 1] = ch.position.y
                }
                InputPump.post { be.touch(display, MotionEvent.ACTION_CANCEL, 0, mouse, buttons, dt, longArrayOf(t), ids, xy) }
                reset()
                ev.changes.forEach { it.consume() }
                continue
            }

            for (ch in ev.changes) {
                if (!ch.changedToDownIgnoreConsumed() || ch.id in pressed) continue

                if (mouse && ev.buttons.isSecondaryPressed) {
                    val t = ch.uptimeMillis
                    InputPump.post {
                        be.key(display, KeyEvent.KEYCODE_BACK, KeyEvent.ACTION_DOWN, 0, t, t)
                        be.key(display, KeyEvent.KEYCODE_BACK, KeyEvent.ACTION_UP, 0, t, t)
                    }
                    swallow = true
                    continue
                }
                if (mouse && pressed.isNotEmpty()) continue
                val t = maxOf(ch.uptimeMillis, lastTime)
                androidId[ch.id] = freeId()
                pressed.add(ch.id)
                val firstContact = pressed.size == 1
                if (firstContact) { downTime = t; c.downTime = t }
                val action = if (firstContact) MotionEvent.ACTION_DOWN else MotionEvent.ACTION_POINTER_DOWN
                val index = pressed.indexOf(ch.id)
                val ids = IntArray(pressed.size) { androidId[pressed[it]]!! }
                val xy = FloatArray(pressed.size * 2)
                pressed.forEachIndexed { i, pid ->
                    val p = ev.changes.firstOrNull { it.id == pid }
                    xy[i * 2] = (p ?: ch).position.x; xy[i * 2 + 1] = (p ?: ch).position.y
                }
                val dt = downTime
                lastTime = t
                InputPump.post { be.touch(display, action, index, mouse, buttons, dt, longArrayOf(t), ids, xy) }
            }

            if (pressed.isNotEmpty() && ev.changes.any { it.id in pressed && it.positionChangedIgnoreConsumed() && !it.changedToDownIgnoreConsumed() && !it.changedToUpIgnoreConsumed() }) {
                val ids = IntArray(pressed.size) { androidId[pressed[it]]!! }
                val n = pressed.size
                val times: LongArray
                val xy: FloatArray
                if (n == 1) {
                    val ch = ev.changes.first { it.id == pressed[0] }
                    val hist = ch.historical
                    times = LongArray(hist.size + 1)
                    xy = FloatArray((hist.size + 1) * 2)
                    var prev = lastTime
                    hist.forEachIndexed { i, h ->
                        val t = maxOf(h.uptimeMillis, prev); prev = t
                        times[i] = t; xy[i * 2] = h.position.x; xy[i * 2 + 1] = h.position.y
                    }
                    val t = maxOf(ch.uptimeMillis, prev)
                    times[hist.size] = t
                    xy[hist.size * 2] = ch.position.x; xy[hist.size * 2 + 1] = ch.position.y
                    lastTime = t
                } else {
                    val t = maxOf(first.uptimeMillis, lastTime)
                    times = longArrayOf(t)
                    xy = FloatArray(n * 2)
                    pressed.forEachIndexed { i, pid ->
                        val p = ev.changes.firstOrNull { it.id == pid }
                        xy[i * 2] = (p ?: first).position.x; xy[i * 2 + 1] = (p ?: first).position.y
                    }
                    lastTime = t
                }
                InputPump.move(be, display, MotionEvent.ACTION_MOVE, mouse, buttons, downTime, ids, times, xy)
            } else if (pressed.isEmpty() && mouse && !swallow && first.positionChangedIgnoreConsumed()) {

                val t = first.uptimeMillis
                InputPump.move(be, display, MotionEvent.ACTION_HOVER_MOVE, true, 0, t, intArrayOf(0),
                    longArrayOf(t), floatArrayOf(first.position.x, first.position.y))
            }

            for (ch in ev.changes) {
                if (!ch.changedToUpIgnoreConsumed()) continue
                if (ch.id !in pressed) { if (mouse) swallow = false; continue }
                val t = maxOf(ch.uptimeMillis, lastTime)
                val index = pressed.indexOf(ch.id)
                val lastContact = pressed.size == 1
                val action = if (lastContact) MotionEvent.ACTION_UP else MotionEvent.ACTION_POINTER_UP
                val ids = IntArray(pressed.size) { androidId[pressed[it]]!! }
                val xy = FloatArray(pressed.size * 2)
                pressed.forEachIndexed { i, pid ->
                    val p = ev.changes.firstOrNull { it.id == pid }
                    xy[i * 2] = (p ?: ch).position.x; xy[i * 2 + 1] = (p ?: ch).position.y
                }
                val dt = downTime
                lastTime = t
                InputPump.post { be.touch(display, action, index, mouse, buttons, dt, longArrayOf(t), ids, xy) }
                pressed.remove(ch.id); androidId.remove(ch.id)
                if (pressed.isEmpty()) swallow = false
            }

            ev.changes.forEach { it.consume() }
        }
    }
}
