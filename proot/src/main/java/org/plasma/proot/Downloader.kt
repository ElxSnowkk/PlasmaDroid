// Parte do PlasmaDroid — módulo PlasmaVK.
// Créditos: baseado no projeto Proot.

package org.plasma.proot

import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

object Downloader {

    private const val MAX_STALLED_ATTEMPTS = 6

    fun download(
        url: String,
        out: File,
        expectedSha256: String?,
        onProgress: (done: Long, total: Long) -> Unit = { _, _ -> },
    ) {
        require(url.startsWith("https://")) { "Apenas https é permitido" }
        out.parentFile?.mkdirs()
        out.delete()

        var total = -1L
        var stalled = 0
        var lastError: IOException? = null
        while (true) {
            val have = if (out.exists()) out.length() else 0L
            if (total in 1..have) break
            try {
                total = fetchFrom(url, out, have, total, onProgress)
                break
            } catch (e: IOException) {
                lastError = e
                val now = if (out.exists()) out.length() else 0L
                stalled = if (now > have) 0 else stalled + 1
                if (stalled >= MAX_STALLED_ATTEMPTS) {
                    out.delete()
                    throw IOException("${e.javaClass.simpleName}: ${e.message} (após $stalled tentativas sem progresso)", e)
                }

                try { Thread.sleep(minOf(1_000L * (stalled + 1), 5_000L)) } catch (_: InterruptedException) {
                    Thread.currentThread().interrupt(); out.delete(); throw e
                }
            }
        }
        if (total > 0 && out.length() != total) {
            val got = out.length()
            out.delete()
            error("Download incompleto ($got de $total bytes)" + (lastError?.let { ": ${it.message}" } ?: ""))
        }
        if (expectedSha256 != null) {
            val md = MessageDigest.getInstance("SHA-256")
            out.inputStream().use { input ->
                val buf = ByteArray(64 * 1024)
                while (true) { val n = input.read(buf); if (n < 0) break; md.update(buf, 0, n) }
            }
            val got = md.digest().joinToString("") { "%02x".format(it) }
            if (!got.equals(expectedSha256, ignoreCase = true)) {
                out.delete()
                error("SHA-256 não confere (esperado $expectedSha256, obtido $got)")
            }
        }
    }

    private fun fetchFrom(url: String, out: File, offset: Long, knownTotal: Long, onProgress: (Long, Long) -> Unit): Long {
        val c = URL(url).openConnection() as HttpURLConnection
        c.connectTimeout = 20_000
        c.readTimeout = 45_000
        c.instanceFollowRedirects = true
        c.setRequestProperty("Accept-Encoding", "identity")
        c.setRequestProperty("User-Agent", "ASDroid")
        if (offset > 0) c.setRequestProperty("Range", "bytes=$offset-")
        try {
            val code = c.responseCode
            var start = offset
            val total: Long
            when (code) {
                200 -> {
                    start = 0
                    total = c.contentLengthLong
                }
                206 -> {
                    val range = c.getHeaderField("Content-Range")
                    total = range?.substringAfter('/')?.trim()?.toLongOrNull() ?: knownTotal
                }
                416 -> {
                    if (knownTotal > 0 && offset >= knownTotal) return knownTotal
                    out.delete(); throw IOException("HTTP 416 ao retomar $url")
                }
                else -> throw IOException("HTTP $code ao baixar $url")
            }
            var done = start
            c.inputStream.use { input ->
                val sink = if (start == 0L) FileOutputStream(out, false) else FileOutputStream(out, true)
                sink.use { fos ->
                    val buf = ByteArray(64 * 1024)
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        fos.write(buf, 0, n)
                        done += n
                        onProgress(done, total)
                    }
                }
            }

            if (total > 0 && done < total) throw IOException("Conexão encerrada em $done de $total bytes")
            return total
        } finally {
            c.disconnect()
        }
    }
}
