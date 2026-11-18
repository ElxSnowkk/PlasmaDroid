
package org.plasma.proot

import android.util.Log
import java.io.File

object AndroidVulkanBridge {
    const val GUEST_DIR = "/usr/lib/plasmavk/android"
    const val GUEST_LIB = "$GUEST_DIR/libvulkan.so"
    private const val TAG = "AndroidVulkanBridge"
    private const val STANDARD = "/system/lib64/libvulkan.so"

    private val DIRS = listOf(
        "/system/lib64", "/system_ext/lib64", "/vendor/lib64", "/product/lib64", "/odm/lib64",
        "/apex/com.android.vndk.current/lib64",
    )

    data class Export(val host: File, val standardOk: Boolean)

    @Volatile private var cached: Export? = null
    @Volatile private var cachedFor: String? = null

    private fun mapped(name: String): List<File> = runCatching {
        File("/proc/self/maps").useLines { lines ->
            lines.mapNotNull { l ->
                l.substringAfterLast(' ', "").takeIf { it.startsWith("/") && it.substringAfterLast('/') == name }
            }.distinct().map { File(it) }.toList()
        }
    }.getOrDefault(emptyList())

    private fun usable(f: File): Boolean = runCatching {
        f.isFile && f.canRead() && f.length() > 0L && BionicDeps.isBionicElf(f) &&
            BionicDeps.exportsSymbol(f, "vkGetInstanceProcAddr")
    }.getOrDefault(false)

    fun locate(): File? = (DIRS.map { File(it, "libvulkan.so") } + mapped("libvulkan.so")).firstOrNull(::usable)

    @Synchronized
    fun prepare(rootfs: File): Export? {
        val key = rootfs.path
        cached?.let { if (cachedFor == key && it.host.isFile) return it }
        val host = locate() ?: run { Log.w(TAG, "nenhuma libvulkan.so Bionic usável no aparelho"); return null }
        val mount = File(rootfs, GUEST_LIB.removePrefix("/"))
        runCatching {
            mount.parentFile?.mkdirs()

            if (!mount.isFile || mount.length() > 0L) mount.writeBytes(ByteArray(0))
        }.onFailure { Log.w(TAG, "não consegui criar o ponto de montagem ${mount.path}", it) }
        val e = Export(host, usable(File(STANDARD)))
        cached = e; cachedFor = key
        Log.i(TAG, "ponte libvulkan: ${host.path} -> $GUEST_LIB (padrão=${e.standardOk})")
        return e
    }

    fun binds(rootfs: File): List<Pair<File, String>> = prepare(rootfs)?.let { listOf(it.host to GUEST_LIB) }.orEmpty()

    fun env(rootfs: File): Map<String, String> {
        val e = prepare(rootfs) ?: return emptyMap()
        val out = LinkedHashMap<String, String>()
        out["PLASMAVK_ANDROID_VULKAN"] = GUEST_LIB
        out["PLASMAVK_ANDROID_VULKAN_HOST"] = e.host.path

        if (!e.standardOk) out["PLASMAVK_DEFAULT_VULKAN_PATH"] = GUEST_LIB
        return out
    }

    fun describe(rootfs: File): String {
        val e = prepare(rootfs) ?: return "libvulkan do Android não encontrada"
        return "${e.host.path} ⇄ $GUEST_LIB (bind do PRoot, sem cópia${if (e.standardOk) "" else "; caminho padrão do wrapper redirecionado"})"
    }
}
