
package org.plasma.app.session

import android.graphics.BitmapFactory
import android.os.Environment
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import org.plasma.app.PlasmaApp
import org.plasma.app.R
import org.plasma.proot.GuestPatches
import java.io.File

data class AppImageInfo(val name: String, val comment: String, val arch: String, val hostArch: String, val icon: ImageBitmap?)

object AppImageSupport {

    fun isAppImage(f: File): Boolean {
        if (!f.isFile) return false
        val ext = f.name.lowercase().endsWith(".appimage")
        return runCatching {
            f.inputStream().use { s ->
                val h = ByteArray(11)
                if (s.read(h) < 11) return@use false
                val elf = h[0] == 0x7f.toByte() && h[1] == 'E'.code.toByte() && h[2] == 'L'.code.toByte() && h[3] == 'F'.code.toByte()
                elf && (ext || (h[8] == 'A'.code.toByte() && h[9] == 'I'.code.toByte()))
            }
        }.getOrDefault(false)
    }

    fun guestPath(f: File): String {
        val root = Environment.getExternalStorageDirectory().absolutePath
        val p = f.absolutePath
        return if (p == root || p.startsWith("$root/")) "/sdcard" + p.removePrefix(root) else p
    }

    fun shq(s: String) = "'" + s.replace("'", "'\\''") + "'"

    fun fallbackIcon(app: PlasmaApp): ImageBitmap? =
        runCatching { BitmapFactory.decodeResource(app.resources, R.drawable.appimage_fallback)?.asImageBitmap() }.getOrNull()

    fun inspect(app: PlasmaApp, f: File): AppImageInfo? {
        val d = app.debian
        if (!d.installed) return null
        runCatching { GuestPatches.install(d.paths.rootfs) }
        val kv = HashMap<String, String>()
        d.exec("/usr/local/bin/plasma-appimage info ${shq(guestPath(f))}", timeoutMs = 150_000L, quiet = true, sink = { l ->
            val i = l.indexOf('='); if (i > 0) kv[l.substring(0, i).trim()] = l.substring(i + 1).trim()
        })
        val iconFile = kv["icon"]?.let { File(d.paths.rootfs, it.removePrefix("/")) }?.takeIf { it.isFile && it.length() > 0 }
        val icon = iconFile?.let { runCatching { BitmapFactory.decodeFile(it.path)?.asImageBitmap() }.getOrNull() }
        return AppImageInfo(kv["name"].orEmpty(), kv["comment"].orEmpty(), kv["arch"].orEmpty(), kv["host_arch"].orEmpty(), icon)
    }

    @Volatile private var pendingIcon: ImageBitmap? = null
    @Volatile private var pendingUntil = 0L

    fun markLaunch(icon: ImageBitmap?) { pendingIcon = icon; pendingUntil = System.currentTimeMillis() + 120_000L }
    fun pending(): ImageBitmap? = if (System.currentTimeMillis() < pendingUntil) pendingIcon else null
}
