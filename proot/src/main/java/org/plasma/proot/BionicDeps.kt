
package org.plasma.proot

import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

object BionicDeps {
    const val GUEST_DIR = "/usr/lib/plasmavk/bionic"

    private val SYSTEM = Regex(
        "^(libc|libm|libdl|liblog|libz|libvulkan|libandroid|libnativewindow|libcutils|libutils|libsync|libEGL|" +
            "libGLESv1_CM|libGLESv2|libGLESv3|libhardware|libbase|libdl_android|ld-android|libnetd_client|libvndksupport)\\.so$",
    )

    data class Report(val wrapper: String?, val missing: List<String>, val glibcWrapper: Boolean) {
        val ok: Boolean get() = wrapper == null || (missing.isEmpty() && !glibcWrapper)
        fun describe(): String = when {
            wrapper == null -> "sem wrapper Bionic"
            glibcWrapper -> "$wrapper é glibc (depende de libc.so.6); o wrapper Bionic tem de ser compilado no Termux"
            missing.isEmpty() -> "dependências Bionic completas"
            else -> "faltam em $GUEST_DIR: ${missing.joinToString()} (rode scripts/build-plasmavk.sh de novo; ele copia libdrm.so e libc++_shared.so)"
        }
    }

    fun check(rootfs: File): Report {
        val wrapperGuest = listOf("libvulkan_wrapper.so", "libwrapper_vulkan.so").map { "$GUEST_DIR/$it" }.firstOrNull { File(rootfs, it.removePrefix("/")).isFile } ?: return Report(null, emptyList(), false)
        val dir = File(rootfs, GUEST_DIR.removePrefix("/"))
        val missing = LinkedHashSet<String>()
        val seen = HashSet<String>()
        var glibc = false
        fun walk(file: File, top: Boolean) {
            val needed = neededOf(file) ?: return
            if (top && needed.contains("libc.so.6")) glibc = true
            for (raw in needed) {
                val name = raw.substringAfterLast('/')
                if (SYSTEM.matches(name) || !seen.add(name)) continue
                val lib = File(dir, name)
                if (lib.isFile && lib.length() > 0L) walk(lib, false) else missing += name
            }
        }
        walk(File(rootfs, wrapperGuest.removePrefix("/")), true)
        return Report(wrapperGuest, missing.toList(), glibc)
    }

    fun isBionicElf(file: File): Boolean = neededOf(file)?.let { "libc.so" in it && "libc.so.6" !in it } ?: false

    fun exportsSymbol(file: File, name: String): Boolean = runCatching {
        val needle = ("\u0000$name\u0000").toByteArray(Charsets.ISO_8859_1)
        val hay = file.readBytes()
        var i = 0
        val last = hay.size - needle.size
        while (i <= last) {
            if (hay[i] == needle[0]) {
                var j = 1
                while (j < needle.size && hay[i + j] == needle[j]) j++
                if (j == needle.size) return@runCatching true
            }
            i++
        }
        false
    }.getOrDefault(false)

    fun missingNeeded(lib: File, extraDirs: List<File>): List<String> {
        val androidDirs = listOf("/vendor/lib64", "/system/lib64", "/odm/lib64", "/vendor/lib64/hw", "/system_ext/lib64").map(::File)
        return (neededOf(lib) ?: emptyList()).map { it.substringAfterLast('/') }
            .filter { n -> !SYSTEM.matches(n) && (extraDirs + androidDirs).none { File(it, n).exists() } }
    }

    fun neededOf(file: File): List<String>? = runCatching {
        RandomAccessFile(file, "r").use { raf ->
            val head = ByteArray(64).also { raf.readFully(it) }
            if (head[0] != 0x7f.toByte() || head[1] != 'E'.code.toByte() || head[2] != 'L'.code.toByte() || head[3] != 'F'.code.toByte()) return null
            if (head[4].toInt() != 2 || head[5].toInt() != 1) return null
            val h = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN)
            val phoff = h.getLong(32); val phentsize = h.getShort(54).toInt() and 0xffff; val phnum = h.getShort(56).toInt() and 0xffff
            class Load(val vaddr: Long, val off: Long, val filesz: Long)
            val loads = ArrayList<Load>()
            var dynOff = -1L; var dynSize = 0L
            for (i in 0 until phnum) {
                val ph = ByteArray(phentsize).also { raf.seek(phoff + i.toLong() * phentsize); raf.readFully(it) }
                val b = ByteBuffer.wrap(ph).order(ByteOrder.LITTLE_ENDIAN)
                when (b.getInt(0)) {
                    1 -> loads += Load(b.getLong(16), b.getLong(8), b.getLong(32))
                    2 -> { dynOff = b.getLong(8); dynSize = b.getLong(32) }
                }
            }
            if (dynOff < 0) return null
            fun vaddrToOff(v: Long): Long = loads.firstOrNull { v >= it.vaddr && v < it.vaddr + it.filesz }?.let { v - it.vaddr + it.off } ?: v
            val dyn = ByteArray(dynSize.toInt()).also { raf.seek(dynOff); raf.readFully(it) }
            val d = ByteBuffer.wrap(dyn).order(ByteOrder.LITTLE_ENDIAN)
            var strtab = -1L
            val names = ArrayList<Long>()
            var p = 0
            while (p + 16 <= dyn.size) {
                val tag = d.getLong(p); val v = d.getLong(p + 8)
                if (tag == 0L) break
                if (tag == 5L) strtab = v
                if (tag == 1L) names += v
                p += 16
            }
            if (strtab < 0) return null
            val base = vaddrToOff(strtab)
            names.map { off ->
                raf.seek(base + off)
                val sb = StringBuilder()
                while (true) { val c = raf.read(); if (c <= 0) break; sb.append(c.toChar()) }
                sb.toString()
            }
        }
    }.getOrNull()
}
