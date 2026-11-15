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

package org.plasma.app.session

import android.app.ActivityManager
import android.content.Context
import android.hardware.display.DisplayManager
import android.os.Build
import android.os.Environment
import android.os.StatFs
import android.os.SystemClock
import org.plasma.app.PlasmaApp
import org.plasma.app.ui.Themes
import org.plasma.core.Keys
import org.plasma.render.RenderBackend
import java.io.File
import java.net.Inet4Address
import java.net.NetworkInterface
import java.util.Locale

object Fastfetch {
    private const val ESC = "\u001b"
    private const val G = ESC + "[1;32m"
    private const val W = ESC + "[1;37m"
    private const val R = ESC + "[0m"

    private val ART = """
         -o          o-
          +hydNNNNdyh+
        +mMMMMMMMMMMMMm+
      `dMM{2}m:{1}NMMMMMMN{2}:m{1}MMd`
      hMMMMMMMMMMMMMMMMMMh
  ..  yyyyyyyyyyyyyyyyyyyy  ..
.mMMm`MMMMMMMMMMMMMMMMMMMM`mMMm.
:MMMM-MMMMMMMMMMMMMMMMMMMM-MMMM:
:MMMM-MMMMMMMMMMMMMMMMMMMM-MMMM:
:MMMM-MMMMMMMMMMMMMMMMMMMM-MMMM:
:MMMM-MMMMMMMMMMMMMMMMMMMM-MMMM:
-MMMM-MMMMMMMMMMMMMMMMMMMM-MMMM-
 +yy+ MMMMMMMMMMMMMMMMMMMM +yy+
      mMMMMMMMMMMMMMMMMMMm
      `/++MMMMh++hMMMM++/`
          MMMMo  oMMMM
          MMMMo  oMMMM
          oNMm-  -mMNs
""".trim('\n')

    private fun bytes(b: Long): String {
        val gib = b / 1073741824.0
        return if (gib >= 1) "%.2f GiB".format(Locale.US, gib) else "%.0f MiB".format(Locale.US, b / 1048576.0)
    }

    private fun uptime(): String {
        var s = SystemClock.elapsedRealtime() / 1000
        val d = s / 86400; s %= 86400
        val h = s / 3600; s %= 3600
        val m = s / 60
        return buildList {
            if (d > 0) add("$d dia" + if (d > 1) "s" else "")
            if (h > 0) add("$h hora" + if (h > 1) "s" else "")
            add("$m min")
        }.joinToString(", ")
    }

    private fun meminfo(key: String): Long? = runCatching {
        File("/proc/meminfo").useLines { ls -> ls.firstOrNull { it.startsWith("$key:") } }
            ?.split(Regex("\\s+"))?.getOrNull(1)?.toLong()?.times(1024)
    }.getOrNull()

    private fun cpu(): String {
        val cores = Runtime.getRuntime().availableProcessors()
        val name = (if (Build.VERSION.SDK_INT >= 31 && Build.SOC_MODEL.isNotBlank() && Build.SOC_MODEL != "unknown")
            (Build.SOC_MANUFACTURER.takeIf { it != "unknown" }?.let { "$it " } ?: "") + Build.SOC_MODEL
        else Build.HARDWARE).replaceFirstChar { it.uppercase() }
        var maxKhz = 0L
        for (i in 0 until cores) runCatching {
            maxKhz = maxOf(maxKhz, File("/sys/devices/system/cpu/cpu$i/cpufreq/cpuinfo_max_freq").readText().trim().toLong())
        }
        return "$name ($cores)" + if (maxKhz > 0) " @ %.2f GHz".format(Locale.US, maxKhz / 1_000_000.0) else ""
    }

    private fun localIp(): String? = runCatching {
        NetworkInterface.getNetworkInterfaces().toList().filter { it.isUp && !it.isLoopback }.firstNotNullOfOrNull { ni ->
            ni.interfaceAddresses.firstOrNull { it.address is Inet4Address }?.let { "${it.address.hostAddress}/${it.networkPrefixLength} (${ni.name})" }
        }
    }.getOrNull()

    fun render(ctx: Context, app: PlasmaApp, s: DesktopSession, user: String, host: String, showLogo: Boolean = true): String {
        val gpu = s.compositor.info.value
        val vk = app.render.selection.value?.probes?.get(RenderBackend.VULKAN)?.takeIf { it.usable }?.details
        val st = app.stats.snapshot()
        val dm = ctx.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
        val disp = dm.getDisplay(s.displayId) ?: dm.getDisplay(0)
        val mode = disp?.mode
        val res = if (mode != null) "${mode.physicalWidth}x${mode.physicalHeight} @ ${Math.round(mode.refreshRate)} Hz" else "?"
        val abi = Build.SUPPORTED_ABIS.firstOrNull()?.replace("arm64-v8a", "aarch64")?.replace("armeabi-v7a", "armv7l") ?: "?"
        val themeLabel = Themes.byId(app.settings.string(Keys.THEME, "breeze-dark")).label
        val ver = runCatching { ctx.packageManager.getPackageInfo(ctx.packageName, 0).versionName }.getOrNull() ?: "?"
        val ext = Environment.getExternalStorageDirectory()
        val sf = runCatching { StatFs(ext.path) }.getOrNull()
        val swapTotal = meminfo("SwapTotal") ?: 0L
        val swapFree = meminfo("SwapFree") ?: 0L
        val memTotal = st.memTotalMb * 1048576L
        val memUsed = (st.memTotalMb - st.memAvailMb) * 1048576L
        val rows = ArrayList<Pair<String, String>>()
        rows += "OS" to "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT}) $abi"
        rows += "Host" to "${Build.MANUFACTURER.replaceFirstChar { it.uppercase() }} ${Build.MODEL} (${Build.DEVICE})"
        rows += "Kernel" to "Linux ${System.getProperty("os.version")}"
        rows += "Uptime" to uptime()
        rows += "Packages" to "${app.apps.value.size} (apk)"
        rows += "Shell" to "sh (toybox)"
        rows += "Display" to "$res [Built-in]"
        rows += "DE" to "KDE Plasma 6 (Android) $ver"
        rows += "WM" to "KWin-compat (OpenGL ES, blur dual-Kawase)"
        rows += "Theme" to "$themeLabel [Plasma]"
        rows += "Icons" to "Breeze [Plasma]"
        rows += "Font" to "Roboto (10pt) [Plasma]"
        rows += "Terminal" to "konsole"
        rows += "CPU" to cpu()
        rows += "GPU" to org.plasma.app.ui.gpuName(s)
        if (gpu.ready) rows += "OpenGL" to gpu.version.removePrefix("OpenGL ES ").let { "ES $it" }
        if (vk != null) rows += "Vulkan" to "${vk["vk.apiMajor"]}.${vk["vk.apiMinor"]} (${vk["vk.device"]})"
        if (gpu.ready) rows += "Compositor" to "OpenGL ES 3 · %.0f fps".format(Locale.US, gpu.fps)
        rows += "Memory" to "${bytes(memUsed)} / ${bytes(memTotal)} (${if (memTotal > 0) memUsed * 100 / memTotal else 0}%)"
        if (swapTotal > 0) rows += "Swap" to "${bytes(swapTotal - swapFree)} / ${bytes(swapTotal)} (${(swapTotal - swapFree) * 100 / swapTotal}%)"
        if (sf != null) {
            val total = sf.totalBytes; val used = total - sf.availableBytes
            rows += "Disk (${ext.path})" to "${bytes(used)} / ${bytes(total)} (${if (total > 0) used * 100 / total else 0}%)"
        }
        if (st.batteryPct >= 0) rows += "Battery" to "${st.batteryPct}% [${if (st.charging) "Charging" else "Discharging"}]"
        localIp()?.let { rows += "Local IP" to it }
        rows += "Locale" to Locale.getDefault().toString() + ".UTF-8"

        val title = "$G$user$R@$G$host$R"
        val info = ArrayList<String>()
        info += title
        info += "-".repeat(user.length + host.length + 1)
        rows.forEach { (k, v) -> info += "$G$k$R: $v" }
        info += ""
        info += (0..7).joinToString("") { "$ESC[4${it}m   " } + R
        info += (0..7).joinToString("") { "$ESC[10${it}m   " } + R

        val logo = if (showLogo) ART.lines().map { it.replace("{1}", G).replace("{2}", W) } else emptyList()
        val logoW = if (showLogo) ART.lines().maxOf { it.replace("{1}", "").replace("{2}", "").length } + 3 else 0
        val sb = StringBuilder()
        val n = maxOf(logo.size, info.size)
        for (i in 0 until n) {
            if (showLogo) {
                val l = logo.getOrNull(i)
                if (l != null) {
                    sb.append(G).append(l).append(R)
                    sb.append(" ".repeat((logoW - l.replace(G, "").replace(W, "").length).coerceAtLeast(1)))
                } else sb.append(" ".repeat(logoW))
            }
            sb.append(info.getOrNull(i) ?: "").append('\n')
        }
        return sb.toString().trimEnd('\n')
    }
}
