
package org.plasma.proot

import java.io.File
import java.io.IOException
import java.nio.file.FileVisitResult
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.Path
import java.nio.file.SimpleFileVisitor
import java.nio.file.attribute.BasicFileAttributes

object SafeFs {

    @Throws(IOException::class)
    fun deleteTree(root: File) {
        val path = root.toPath()
        if (!Files.exists(path, LinkOption.NOFOLLOW_LINKS)) return
        var last: IOException? = null
        repeat(3) { attempt ->
            try {
                Files.walkFileTree(path, object : SimpleFileVisitor<Path>() {
                    override fun visitFile(file: Path, attrs: BasicFileAttributes): FileVisitResult {
                        Files.delete(file)
                        return FileVisitResult.CONTINUE
                    }

                    override fun visitFileFailed(file: Path, exc: IOException): FileVisitResult {

                        try {
                            Files.deleteIfExists(file)
                            return FileVisitResult.CONTINUE
                        } catch (deleteError: IOException) {
                            deleteError.addSuppressed(exc)
                            throw deleteError
                        }
                    }

                    override fun postVisitDirectory(dir: Path, exc: IOException?): FileVisitResult {
                        if (exc != null) throw exc
                        Files.delete(dir)
                        return FileVisitResult.CONTINUE
                    }
                })
                if (!Files.exists(path, LinkOption.NOFOLLOW_LINKS)) return
                last = IOException("O caminho ainda existe após a exclusão: $root")
            } catch (e: IOException) {
                last = e
            }
            if (attempt < 2) try { Thread.sleep(80L * (attempt + 1)) } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
                throw IOException("Exclusão interrompida: $root", last)
            }
        }
        throw IOException("Não foi possível apagar completamente $root: ${last?.message}", last)
    }

    fun sizeOf(root: File): Long {
        val p = try { root.toPath() } catch (_: SecurityException) { return 0L }
        try {
            if (!Files.exists(p, LinkOption.NOFOLLOW_LINKS)) return 0L
        } catch (_: SecurityException) {
            return 0L
        }

        var total = 0L
        try {
            Files.walkFileTree(p, object : SimpleFileVisitor<Path>() {
                override fun visitFile(file: Path, attrs: BasicFileAttributes): FileVisitResult {
                    if (attrs.isRegularFile) {
                        val size = attrs.size().coerceAtLeast(0L)
                        total = if (Long.MAX_VALUE - total < size) Long.MAX_VALUE else total + size
                    }
                    return FileVisitResult.CONTINUE
                }

                override fun visitFileFailed(file: Path, exc: IOException): FileVisitResult {

                    return FileVisitResult.CONTINUE
                }

                override fun postVisitDirectory(dir: Path, exc: IOException?): FileVisitResult {

                    return FileVisitResult.CONTINUE
                }
            })
        } catch (_: IOException) {

        } catch (_: SecurityException) {

        }
        return total
    }
}
