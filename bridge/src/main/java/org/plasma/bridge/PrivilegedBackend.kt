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

package org.plasma.bridge

import android.os.Build
import android.view.Surface
import org.plasma.core.PlasmaLog

interface PrivilegedBackend {
    val name: String
    val trustedDisplays: Boolean
    fun createDisplay(name: String, w: Int, h: Int, dpi: Int, surface: Surface): Int
    fun releaseDisplay(id: Int)
    fun resizeDisplay(id: Int, w: Int, h: Int, dpi: Int)
    fun setDisplaySurface(id: Int, surface: Surface)
    fun launch(displayId: Int, component: String): String?
    fun motion(displayId: Int, action: Int, x: Float, y: Float, downTime: Long, eventTime: Long, mouse: Boolean)
    fun key(displayId: Int, keyCode: Int, action: Int, meta: Int, downTime: Long, eventTime: Long)

    fun touch(displayId: Int, action: Int, actionIndex: Int, mouse: Boolean, buttonState: Int, downTime: Long,
              times: LongArray, ids: IntArray, xy: FloatArray) {
        if (times.isEmpty() || ids.isEmpty()) return
        val k = times.size - 1
        val i = 0
        motion(displayId, action and 0xFF, xy[(k * ids.size + i) * 2], xy[(k * ids.size + i) * 2 + 1], downTime, times[k], mouse)
    }
    fun scroll(displayId: Int, x: Float, y: Float, v: Float, h: Float, time: Long)
    fun exec(cmd: List<String>, timeoutMs: Int = 6000): String

    fun bluetooth(op: String, address: String, timeoutMs: Int = 9000): String = "erro: indisponível neste backend"
    fun lastError(): String
}

object DisplayFlags {
    const val PUBLIC = 1 shl 0
    const val OWN_CONTENT_ONLY = 1 shl 3
    const val SUPPORTS_TOUCH = 1 shl 6
    const val ROTATES_WITH_CONTENT = 1 shl 7
    const val DESTROY_CONTENT_ON_REMOVAL = 1 shl 8
    const val TRUSTED = 1 shl 10
    const val OWN_DISPLAY_GROUP = 1 shl 11
    const val ALWAYS_UNLOCKED = 1 shl 12
    const val TOUCH_FEEDBACK_DISABLED = 1 shl 13
    const val OWN_FOCUS = 1 shl 14

    fun trusted(sdk: Int = Build.VERSION.SDK_INT): Int {
        var f = PUBLIC or OWN_CONTENT_ONLY or SUPPORTS_TOUCH or ROTATES_WITH_CONTENT or DESTROY_CONTENT_ON_REMOVAL
        if (sdk >= 33) f = f or TRUSTED or OWN_DISPLAY_GROUP or ALWAYS_UNLOCKED or TOUCH_FEEDBACK_DISABLED
        if (sdk >= 34) f = f or OWN_FOCUS
        return f
    }
}

class ShizukuBackend(private val svc: IPrivilegedService) : PrivilegedBackend {
    override val name = "Shizuku"
    override val trustedDisplays = true
    private var err = ""

    private inline fun <T> safe(def: T, block: () -> T): T = try { block() } catch (t: Throwable) {
        err = t.toString(); PlasmaLog.w("shizuku", "chamada falhou", t); def
    }

    override fun createDisplay(name: String, w: Int, h: Int, dpi: Int, surface: Surface): Int = safe(-1) {
        val id = svc.createDisplay(name, w, h, dpi, surface, DisplayFlags.trusted())
        if (id < 0) err = svc.lastError()
        id
    }
    override fun releaseDisplay(id: Int) = safe(Unit) { svc.releaseDisplay(id) }
    override fun resizeDisplay(id: Int, w: Int, h: Int, dpi: Int) = safe(Unit) { svc.resizeDisplay(id, w, h, dpi) }
    override fun setDisplaySurface(id: Int, surface: Surface) = safe(Unit) { svc.setDisplaySurface(id, surface) }

    override fun launch(displayId: Int, component: String): String? = safe("falha de binder") {
        val out = svc.exec(arrayOf("am", "start", "--display", "$displayId", "-f", "402653184", "-n", component), 8000)
        PlasmaLog.i("shizuku", "am start --display $displayId -n $component -> ${out.replace('\n', ' ').take(200)}")
        if (out.contains("Error", true) || out.contains("Exception") || !out.startsWith("exit=0")) out.take(300) else null
    }

    @Volatile private var shellInput = false
    @Volatile private var shellInputSince = 0L
    private var lastShellMove = 0L
    private var firstMotionChecked = false

    override fun motion(displayId: Int, action: Int, x: Float, y: Float, downTime: Long, eventTime: Long, mouse: Boolean) {
        val now = System.currentTimeMillis()

        if (shellInput && action == 0 && now - shellInputSince > 15_000) shellInput = false
        if (shellInput && action == 2) {
            if (now - lastShellMove < 150) return
            lastShellMove = now
        }
        if (!shellInput) {
            val accepted = safe(false) {
                svc.injectMotion(displayId, action, x, y, downTime, eventTime, if (mouse) 0x2002 else 0x1002)
            }
            if (accepted) {
                if (action == 0 && !firstMotionChecked) {
                    firstMotionChecked = true
                    PlasmaLog.i("shizuku", "primeiro toque no display $displayId: InputManager aceitou")
                }
                return
            }
            if (action == 7 || action == 2) return
            val reason = safe("InputManager não aceitou o evento") { svc.lastError() }
            err = reason
            if (action == 1) {
                val fallback = safe("exit=error") {
                    svc.exec(arrayOf("input", "-d", "$displayId", "tap", "${x.toInt()}", "${y.toInt()}"), 1500)
                }
                if (!fallback.startsWith("exit=0")) err = fallback
                PlasmaLog.w("shizuku", "UP rejeitado no display $displayId; fallback tap (${fallback.substringBefore('\n')}) ($reason)")
                return
            }
            if (action != 0) {
                PlasmaLog.w("shizuku", "evento $action rejeitado no display $displayId ($reason)")
                return
            }
            shellInput = true
            shellInputSince = now
            PlasmaLog.w("shizuku", "injeção direta rejeitada no display $displayId ($reason); ativando fallback `input`")
        }

        val a = when (action) {
            0 -> "DOWN"
            1 -> "UP"
            2 -> "MOVE"
            3 -> "UP"
            7 -> return
            else -> return
        }
        val out = safe("exit=error") {
            svc.exec(arrayOf("input", "-d", "$displayId", "motionevent", a, "${x.toInt()}", "${y.toInt()}"), 1500)
        }
        if (!out.startsWith("exit=0")) {
            err = out
            PlasmaLog.w("shizuku", "fallback input recusado no display $displayId: ${out.replace('\n', ' ').take(220)}")
        }
    }
    override fun touch(displayId: Int, action: Int, actionIndex: Int, mouse: Boolean, buttonState: Int, downTime: Long,
                       times: LongArray, ids: IntArray, xy: FloatArray) {
        if (action == 0) logStatsIfDue()

        safe(Unit) { svc.injectTouch(displayId, action, actionIndex, if (mouse) 0x2002 else 0x1002, buttonState, downTime, times, ids, xy) }
    }
    private var lastStats = 0L

    private fun logStatsIfDue() {
        val now = System.currentTimeMillis()
        if (now - lastStats < 5000) return
        lastStats = now
        PlasmaLog.i("input", safe("indisponível") { svc.inputStats() })
    }
    override fun key(displayId: Int, keyCode: Int, action: Int, meta: Int, downTime: Long, eventTime: Long) =
        safe(Unit) { svc.injectKey(displayId, keyCode, action, meta, downTime, eventTime) }
    override fun scroll(displayId: Int, x: Float, y: Float, v: Float, h: Float, time: Long) =
        safe(Unit) { svc.injectScroll(displayId, x, y, v, h, time) }
    override fun exec(cmd: List<String>, timeoutMs: Int): String = safe("erro") { svc.exec(cmd.toTypedArray(), timeoutMs) }
    override fun bluetooth(op: String, address: String, timeoutMs: Int): String =
        safe("erro: falha de binder") { svc.bluetooth(op, address, timeoutMs) }
    override fun lastError(): String = err
}

class RootBackend(private val run: (List<String>, Int) -> String) : PrivilegedBackend {
    override val name = "Root"
    override val trustedDisplays = false
    private var err = ""
    override fun createDisplay(name: String, w: Int, h: Int, dpi: Int, surface: Surface) = -1
    override fun releaseDisplay(id: Int) {}
    override fun resizeDisplay(id: Int, w: Int, h: Int, dpi: Int) {}
    override fun setDisplaySurface(id: Int, surface: Surface) {}
    override fun launch(displayId: Int, component: String): String? {
        val out = run(listOf("am", "start", "--display", "$displayId", "-f", "402653184", "-n", component), 8000)
        return if (out.contains("Error", true) || out.contains("Exception")) out.take(300) else null
    }
    override fun motion(displayId: Int, action: Int, x: Float, y: Float, downTime: Long, eventTime: Long, mouse: Boolean) {
        val xi = x.toInt().toString(); val yi = y.toInt().toString()
        if (Build.VERSION.SDK_INT >= 31) {
            val a = when (action) { 0 -> "DOWN"; 1 -> "UP"; 2 -> "MOVE"; else -> return }
            run(listOf("input", "-d", "$displayId", "motionevent", a, xi, yi), 1500)
        } else if (action == 1) run(listOf("input", "-d", "$displayId", "tap", xi, yi), 1500)
    }
    override fun key(displayId: Int, keyCode: Int, action: Int, meta: Int, downTime: Long, eventTime: Long) {
        if (action == 0) run(listOf("input", "-d", "$displayId", "keyevent", "$keyCode"), 1500)
    }
    override fun scroll(displayId: Int, x: Float, y: Float, v: Float, h: Float, time: Long) {
        val d = (v * -200).toInt()
        run(listOf("input", "-d", "$displayId", "swipe", "${x.toInt()}", "${y.toInt()}", "${x.toInt()}", "${y.toInt() + d}", "120"), 1500)
    }
    override fun exec(cmd: List<String>, timeoutMs: Int): String = run(cmd, timeoutMs)
    override fun lastError(): String = err
}
