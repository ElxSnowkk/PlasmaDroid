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

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Debug
import android.os.Process
import android.os.SystemClock
import java.io.File

data class SysSnapshot(
    val memTotalMb: Long,
    val memAvailMb: Long,
    val appPssMb: Long,
    val batteryPct: Int,
    val charging: Boolean,
    val batteryTempC: Float?,
    val gpuBusyPct: Int?,
    val cpuCores: Int,
    val appCpuPct: Float,
)

class SystemStats(private val ctx: Context) {
    private var lastCpu = Process.getElapsedCpuTime()
    private var lastWall = SystemClock.elapsedRealtime()

    fun snapshot(): SysSnapshot {
        val am = ctx.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val mi = ActivityManager.MemoryInfo().also { am.getMemoryInfo(it) }
        val dbg = Debug.MemoryInfo().also { Debug.getMemoryInfo(it) }
        val bat: Intent? = ctx.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = bat?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = bat?.getIntExtra(BatteryManager.EXTRA_SCALE, 100) ?: 100
        val status = bat?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
        val temp = bat?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE) ?: Int.MIN_VALUE
        val cpu = Process.getElapsedCpuTime()
        val wall = SystemClock.elapsedRealtime()
        val pct = if (wall > lastWall) (cpu - lastCpu) * 100f / (wall - lastWall) else 0f
        lastCpu = cpu; lastWall = wall
        return SysSnapshot(
            memTotalMb = mi.totalMem / MB,
            memAvailMb = mi.availMem / MB,
            appPssMb = dbg.totalPss / 1024L,
            batteryPct = if (level >= 0) level * 100 / scale else -1,
            charging = status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL,
            batteryTempC = if (temp != Int.MIN_VALUE) temp / 10f else null,
            gpuBusyPct = readGpuBusy(),
            cpuCores = Runtime.getRuntime().availableProcessors(),
            appCpuPct = pct,
        )
    }

    private fun readGpuBusy(): Int? {
        runCatching {
            val t = File("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage").readText().trim().removeSuffix("%").trim()
            return t.toInt()
        }
        runCatching {
            val p = File("/sys/class/kgsl/kgsl-3d0/gpubusy").readText().trim().split(Regex("\\s+"))
            val busy = p[0].toLong(); val total = p[1].toLong()
            if (total > 0) return (busy * 100 / total).toInt()
        }
        return null
    }

    private companion object { const val MB = 1024L * 1024L }
}
