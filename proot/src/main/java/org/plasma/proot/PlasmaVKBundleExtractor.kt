// Parte do PlasmaDroid — módulo PlasmaVK.
// Créditos: baseado no projeto Proot.

package org.plasma.proot

import java.io.BufferedInputStream
import java.io.File
import java.io.FileInputStream
import java.io.FilterInputStream
import java.io.InputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.Properties
import java.util.zip.ZipInputStream

internal object PlasmaVKBundleExtractor {
    private val MAGIC = byteArrayOf('A'.code.toByte(), 'S'.code.toByte(), 'D'.code.toByte(), 'P'.code.toByte(), 'R'.code.toByte(), 'K'.code.toByte(), 'R'.code.toByte(), '1'.code.toByte())
    private const val FOOTER_SIZE = 56
    private const val MAX_PAYLOAD_BYTES = 768L * 1024 * 1024
    private const val MAX_EXPANDED_BYTES = 1536L * 1024 * 1024
    private const val MAX_ENTRY_BYTES = 512L * 1024 * 1024
    private const val MAX_ENTRIES = 30_000

    data class Metadata(
        val backend: String,
        val manifest: String,
        val libraryDirs: List<String>,
    )

    @Synchronized
    fun extract(executable: File, targetRoot: File): Metadata? {
        if (!executable.isFile || executable.length() < FOOTER_SIZE + 4) return null
        val footer = ByteArray(FOOTER_SIZE)
        RandomAccessFile(executable, "r").use { raf ->
            raf.seek(0)
            val elfMagic = ByteArray(4)
            raf.readFully(elfMagic)
            if (!elfMagic.contentEquals(byteArrayOf(0x7f, 'E'.code.toByte(), 'L'.code.toByte(), 'F'.code.toByte()))) {
                throw IllegalStateException("libproot.so não começa com um cabeçalho ELF válido")
            }
            raf.seek(raf.length() - FOOTER_SIZE)
            raf.readFully(footer)
        }
        if (!footer.copyOfRange(0, MAGIC.size).contentEquals(MAGIC)) return null

        val bb = ByteBuffer.wrap(footer).order(ByteOrder.BIG_ENDIAN)
        bb.position(8)
        val offset = bb.long
        val length = bb.long
        val expectedDigest = ByteArray(32).also { bb.get(it) }
        val fileLength = executable.length()
        require(offset >= 4 && offset < fileLength && length > 0 && length <= MAX_PAYLOAD_BYTES &&
            offset <= fileLength - FOOTER_SIZE && length == fileLength - FOOTER_SIZE - offset) {
            "Payload PlasmaVK tem tamanho/offset inválido"
        }
        val digest = digestRange(executable, offset, length)
        require(MessageDigest.isEqual(digest, expectedDigest)) { "Hash SHA-256 do payload PlasmaVK não confere" }
        val digestHex = digest.joinToString("") { (it.toInt() and 0xff).toString(16).padStart(2, '0') }
        val completionMarker = File(targetRoot, ".plasmavk-complete")
        if (completionMarker.isFile && runCatching { completionMarker.readText().trim() }.getOrNull() == digestHex && requiredHostFilesExist(targetRoot)) {
            return readMetadata(targetRoot)
        }

        clearDirectory(targetRoot)
        check(targetRoot.mkdirs() || targetRoot.isDirectory) { "Não foi possível criar diretório do payload PlasmaVK" }
        FileInputStream(executable).use { fileIn ->
            skipFully(fileIn, offset)
            val limited = LimitedInputStream(fileIn, length)
            ZipInputStream(BufferedInputStream(limited)).use { zip ->
                var entries = 0
                var expanded = 0L
                while (true) {
                    val entry = zip.nextEntry ?: break
                    entries++
                    require(entries <= MAX_ENTRIES) { "Payload PlasmaVK contém arquivos demais" }
                    val relative = safeRelativePath(entry.name)
                    val output = File(targetRoot, relative)
                    val canonicalRoot = targetRoot.canonicalFile.toPath()
                    val canonicalOutput = output.canonicalFile.toPath()
                    require(canonicalOutput.startsWith(canonicalRoot)) { "Caminho ZIP fora do payload: ${entry.name}" }
                    if (entry.isDirectory) {
                        check(output.mkdirs() || output.isDirectory) { "Falha criando ${entry.name}" }
                    } else {
                        val parent = output.parentFile
                        check(parent == null || parent.isDirectory || parent.mkdirs()) { "Falha criando pasta para ${entry.name}" }
                        var entryBytes = 0L
                        output.outputStream().buffered().use { out ->
                            val buffer = ByteArray(32 * 1024)
                            while (true) {
                                val n = zip.read(buffer)
                                if (n < 0) break
                                entryBytes += n
                                expanded += n
                                require(entryBytes <= MAX_ENTRY_BYTES && expanded <= MAX_EXPANDED_BYTES) {
                                    "Payload PlasmaVK excedeu o limite de extração"
                                }
                                out.write(buffer, 0, n)
                            }
                        }
                        if (relative == "host/libproot-loader.so") output.setExecutable(true, false)
                        if (relative.startsWith("host/") && relative.endsWith(".so")) output.setReadable(true, false)
                    }
                    zip.closeEntry()
                }
            }
        }
        check(requiredHostFilesExist(targetRoot)) { "Payload PlasmaVK incompleto: loader/talloc ausentes" }
        completionMarker.writeText(digestHex)
        return readMetadata(targetRoot)
    }

    fun readMetadata(targetRoot: File): Metadata? {
        val file = File(targetRoot, "META-INF/plasmavk-defaults.properties")
        if (!file.isFile) return null
        val p = Properties()
        file.inputStream().use { p.load(it) }
        val backend = p.getProperty("backend")?.trim().orEmpty()
        val manifest = p.getProperty("manifest")?.trim().orEmpty()
        val dirs = p.getProperty("libraryDirs")?.split(':')?.map(String::trim)?.filter(String::isNotEmpty).orEmpty()
        if (backend.isBlank() || manifest.isBlank() || !manifest.startsWith('/') || dirs.any { !it.startsWith('/') }) return null
        return Metadata(backend, manifest, dirs)
    }

    private fun requiredHostFilesExist(root: File): Boolean =
        File(root, "host/libproot-loader.so").isFile && File(root, "host/libtalloc.so").isFile

    private fun digestRange(file: File, offset: Long, length: Long): ByteArray {
        val md = MessageDigest.getInstance("SHA-256")
        RandomAccessFile(file, "r").use { raf ->
            raf.seek(offset)
            var remaining = length
            val buffer = ByteArray(128 * 1024)
            while (remaining > 0) {
                val count = raf.read(buffer, 0, minOf(buffer.size.toLong(), remaining).toInt())
                if (count <= 0) throw IllegalStateException("Payload PlasmaVK truncado")
                md.update(buffer, 0, count)
                remaining -= count
            }
        }
        return md.digest()
    }

    private fun skipFully(input: InputStream, amount: Long) {
        var remaining = amount
        while (remaining > 0) {
            val skipped = input.skip(remaining)
            if (skipped > 0) remaining -= skipped else {
                if (input.read() < 0) throw IllegalStateException("EOF ao localizar payload PlasmaVK")
                remaining--
            }
        }
    }

    private fun safeRelativePath(name: String): String {
        require(name.isNotBlank() && '\u0000' !in name && '\\' !in name && !name.startsWith('/') && !Regex("^[A-Za-z]:").containsMatchIn(name)) {
            "Caminho ZIP inválido: $name"
        }
        val parts = name.split('/')
        require(parts.none { it == ".." || it == "." || it.isBlank() }) { "Caminho ZIP inválido: $name" }
        return parts.joinToString(File.separator)
    }

    private fun clearDirectory(root: File) {
        if (!root.exists()) return
        root.listFiles()?.forEach { child ->
            if (child.isDirectory && !child.isSymlink()) clearDirectory(child)
            if (!child.delete()) throw IllegalStateException("Não foi possível limpar ${child.absolutePath}")
        }
    }

    private fun File.isSymlink(): Boolean = runCatching {
        val path = toPath()
        java.nio.file.Files.isSymbolicLink(path)
    }.getOrDefault(false)

    private class LimitedInputStream(input: InputStream, private var remaining: Long) : FilterInputStream(input) {
        override fun read(): Int {
            if (remaining <= 0) return -1
            val value = super.read()
            if (value >= 0) remaining--
            return value
        }
        override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
            if (remaining <= 0) return -1
            val count = super.read(buffer, offset, minOf(length.toLong(), remaining).toInt())
            if (count > 0) remaining -= count
            return count
        }
        override fun skip(n: Long): Long {
            val count = super.skip(minOf(n, remaining))
            remaining -= count
            return count
        }
        override fun close() {  }
    }
}
