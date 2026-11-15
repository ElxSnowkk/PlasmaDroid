// Parte do PlasmaDroid — módulo PlasmaVK.
// Créditos: baseado no projeto Proot.

package org.plasma.proot

import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import org.apache.commons.compress.compressors.xz.XZCompressorInputStream
import java.io.BufferedInputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.InputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.zip.GZIPInputStream

object RootfsExtractor {
    class UnsafeEntryException(message: String) : Exception(message)

    data class Limits(
        val maxEntries: Int = 500_000,
        val maxTotalBytes: Long = 8L * 1024 * 1024 * 1024,
        val maxEntryBytes: Long = 4L * 1024 * 1024 * 1024,
    )

    data class Result(
        val files: Int, val dirs: Int, val symlinks: Int,
        val hardlinks: Int, val skipped: Int, val bytes: Long,
    )

    fun normalize(name: String): String? {
        var n = name
        while (n.startsWith("./")) n = n.substring(2)
        n = n.trimEnd('/')
        if (n.isEmpty() || n == ".") return null

        if (n.startsWith("/") || n.contains('\u0000')) {
            throw UnsafeEntryException("Nome de entrada inválido: $name")
        }
        return n
    }

    fun safeResolve(root: File, name: String): File {
        val rootCanon = root.canonicalFile
        val target = File(rootCanon, name).canonicalFile
        val ok = target.path == rootCanon.path || target.path.startsWith(rootCanon.path + File.separator)
        if (!ok) throw UnsafeEntryException("Entrada fora do destino: $name")
        return target
    }

    fun openDecompressed(file: File): InputStream {
        val head = ByteArray(6)
        val n = FileInputStream(file).use { it.read(head) }
        val raw = BufferedInputStream(FileInputStream(file), 64 * 1024)
        fun b(i: Int, v: Int) = head[i] == v.toByte()
        return when {
            n >= 2 && b(0, 0x1f) && b(1, 0x8b) -> GZIPInputStream(raw, 64 * 1024)
            n >= 6 && b(0, 0xFD) && b(1, 0x37) && b(2, 0x7A) && b(3, 0x58) && b(4, 0x5A) ->
                XZCompressorInputStream(raw)
            n >= 4 && b(0, 0x28) && b(1, 0xB5) && b(2, 0x2F) && b(3, 0xFD) -> {
                raw.close()
                throw IllegalArgumentException("Formato .zst não suportado. Use .tar.gz ou .tar.xz.")
            }
            else -> raw
        }
    }

    fun extract(
        archive: File,
        dest: File,
        limits: Limits = Limits(),
        symlinker: (target: String, link: File) -> Unit,
        onProgress: (entries: Int) -> Unit = {},
    ): Result {
        dest.mkdirs()
        val hard = ArrayList<Pair<String, String>>()
        val soft = ArrayList<Pair<String, String>>()
        var entries = 0
        var files = 0
        var dirs = 0
        var skipped = 0
        var total = 0L

        TarArchiveInputStream(openDecompressed(archive)).use { tar ->
            var e = tar.nextEntry as TarArchiveEntry?
            while (e != null) {
                entries++
                if (entries > limits.maxEntries) throw UnsafeEntryException("Entradas demais")
                val name = normalize(e.name)
                if (name != null) {
                    when {
                        e.isDirectory -> {
                            val t = safeResolve(dest, name)
                            t.mkdirs()
                            t.setReadable(true, true); t.setWritable(true, true); t.setExecutable(true, true)
                            dirs++
                        }
                        e.isSymbolicLink -> soft += name to e.linkName
                        e.isLink -> {
                            val target = normalize(e.linkName)
                                ?: throw UnsafeEntryException("Hardlink inválido: ${e.name}")
                            hard += name to target
                        }
                        e.isFile -> {
                            val t = safeResolve(dest, name)
                            t.parentFile?.mkdirs()
                            var entryBytes = 0L
                            FileOutputStream(t).use { out ->
                                val buf = ByteArray(64 * 1024)
                                while (true) {
                                    val n = tar.read(buf)
                                    if (n < 0) break
                                    entryBytes += n
                                    total += n
                                    if (entryBytes > limits.maxEntryBytes || total > limits.maxTotalBytes) {
                                        throw UnsafeEntryException("Limite de tamanho excedido em $name")
                                    }
                                    out.write(buf, 0, n)
                                }
                            }
                            t.setReadable(true, true)
                            t.setWritable(true, true)
                            t.setExecutable((e.mode and 0b001_001_001) != 0, true)
                            files++
                        }
                        else -> skipped++
                    }
                }
                onProgress(entries)
                e = tar.nextEntry as TarArchiveEntry?
            }
        }

        val deferred = ArrayList<Pair<String, String>>()
        fun linkCopy(name: String, target: String): Boolean {
            val src = safeResolve(dest, target)
            if (!src.isFile) return false
            val dst = safeResolve(dest, name)
            dst.parentFile?.mkdirs()
            Files.copy(src.toPath(), dst.toPath(), StandardCopyOption.REPLACE_EXISTING)
            dst.setExecutable(src.canExecute(), true)
            return true
        }
        for ((name, target) in hard) if (!linkCopy(name, target)) deferred += name to target

        for ((name, target) in soft) {
            val parentRel = name.substringBeforeLast('/', "")
            val parent = if (parentRel.isEmpty()) dest.canonicalFile else safeResolve(dest, parentRel)
            parent.mkdirs()
            val link = File(parent, name.substringAfterLast('/'))
            Files.deleteIfExists(link.toPath())
            symlinker(target, link)
        }

        var pending = deferred
        while (pending.isNotEmpty()) {
            val still = ArrayList<Pair<String, String>>()
            for ((name, target) in pending) if (!linkCopy(name, target)) still += name to target
            if (still.size == pending.size) throw UnsafeEntryException("Alvo de hardlink inexistente: ${still.first().second}")
            pending = still
        }
        return Result(files, dirs, soft.size, hard.size, skipped, total)
    }
}
