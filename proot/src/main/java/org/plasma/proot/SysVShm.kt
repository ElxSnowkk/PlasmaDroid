// Parte do PlasmaDroid — módulo PlasmaVK.
// Créditos: baseado no projeto Proot.

package org.plasma.proot

import android.content.Context
import android.system.Os
import org.plasma.core.PlasmaLog
import java.io.File
import kotlin.concurrent.thread

object SysVShm {
    private const val TAG = "SysVShm"
    const val GUEST_LIB = "/usr/local/lib/libandroid-sysvshm.so"
    const val GUEST_SOCKET = "/tmp/.plasma-shm/sock"
    private const val ASSET = "sysvshm/libandroid-sysvshm.so"
    private const val SOCK_DIR = "tmp/.plasma-shm"

    private val servers = HashMap<String, Process>()
    private var warnedNoAsset = false

    fun disabled(p: ProotPaths): Boolean = File(p.root, "debian/.no-sysvshm").exists()

    @Synchronized fun installClient(ctx: Context, rootfs: File): Boolean {
        val bytes = runCatching { ctx.assets.open(ASSET).use { it.readBytes() } }.getOrNull()
        if (bytes == null || bytes.isEmpty()) {
            if (!warnedNoAsset) { warnedNoAsset = true; PlasmaLog.w(TAG, "assets/$ASSET ausente: rode scripts/build-sysvshm.sh e recompile o APK") }
            return false
        }
        val dest = File(rootfs, GUEST_LIB.removePrefix("/"))
        return runCatching {
            dest.parentFile?.mkdirs()
            if (!dest.isFile || dest.length() != bytes.size.toLong() || !dest.readBytes().contentEquals(bytes)) {
                val tmp = File(dest.parentFile, dest.name + ".new")
                tmp.writeBytes(bytes)
                tmp.setReadable(true, false); tmp.setExecutable(true, false)
                if (!tmp.renameTo(dest)) { tmp.delete(); error("não consegui gravar ${dest.path}") }
            }
            true
        }.onFailure { PlasmaLog.e(TAG, "instalar cliente: ${it.message}", it) }.getOrDefault(false)
    }

    @Synchronized fun ensureServer(p: ProotPaths, rootfs: File): Boolean {
        if (!p.sysvshmServer.exists()) return false
        val dir = File(rootfs, SOCK_DIR)
        val sock = File(dir, "sock")
        val key = rootfs.path
        val cur = servers[key]
        if (cur != null && cur.isAlive && sock.exists()) return true
        if (sock.path.length >= 108) { PlasmaLog.e(TAG, "caminho do socket longo demais: ${sock.path}"); return false }

        cur?.destroy()
        runCatching {
            dir.mkdirs()
            Os.chmod(dir.path, 0x1C0)
            sock.delete()
            if (!p.sysvshmServer.canExecute()) p.sysvshmServer.setExecutable(true, false)
        }
        val proc = runCatching {
            ProcessBuilder(p.sysvshmServer.path, sock.path, "-f").redirectErrorStream(true)
                .apply { environment().clear() }.start()
        }.onFailure { PlasmaLog.e(TAG, "iniciar broker: ${it.message}", it) }.getOrNull() ?: return false
        servers[key] = proc
        thread(isDaemon = true, name = "sysvshm-log") {
            runCatching { proc.inputStream.bufferedReader().forEachLine { PlasmaLog.d(TAG, it) } }
        }
        repeat(60) {
            if (sock.exists()) return true
            if (!proc.isAlive) { PlasmaLog.e(TAG, "broker encerrou (código ${runCatching { proc.exitValue() }.getOrNull()})"); return false }
            Thread.sleep(50)
        }
        return sock.exists()
    }

    fun guestEnv(p: ProotPaths, rootfs: File): Map<String, String> {
        if (disabled(p)) return emptyMap()
        if (!File(rootfs, GUEST_LIB.removePrefix("/")).isFile) return emptyMap()
        if (!ensureServer(p, rootfs)) return emptyMap()
        return mapOf("LD_PRELOAD" to GUEST_LIB, "ANDROID_SYSVSHM_SERVER" to GUEST_SOCKET)
    }

    fun mergePreload(a: String?, b: String?): String =
        (a.orEmpty().split(':', ' ') + b.orEmpty().split(':', ' ')).filter { it.isNotBlank() }.distinct().joinToString(":")

    @Synchronized fun stop() {
        servers.values.forEach { runCatching { it.destroy() } }
        servers.clear()
    }
}
