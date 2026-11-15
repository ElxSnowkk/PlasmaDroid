// Parte do PlasmaDroid — módulo PlasmaVK.
// Créditos: baseado no projeto Proot.

package org.plasma.proot

import android.util.Base64
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

object DebianCatalog {
    const val DEBIAN_BASE = "https://raw.githubusercontent.com/debuerreotype/docker-debian-artifacts/dist-arm64v8/"

    val SUITES = listOf("trixie", "bookworm")

    data class Rootfs(val url: String, val file: String, val sha256: String?, val suite: String)

    fun fetchText(url: String): String {
        val c = URL(url).openConnection() as HttpURLConnection
        c.connectTimeout = 15_000; c.readTimeout = 30_000
        try {
            check(c.responseCode == 200) { "HTTP ${c.responseCode} ao consultar $url" }
            return c.inputStream.bufferedReader().use { it.readText() }
        } finally { c.disconnect() }
    }

    private fun resolve(suite: String): Rootfs {
        val base = "${DEBIAN_BASE}$suite/slim/oci/"
        val index = JSONObject(fetchText(base + "index.json"))
        val manifests = index.optJSONArray("manifests") ?: error("Índice OCI Debian $suite sem manifests")
        var descriptor: JSONObject? = null
        for (i in 0 until manifests.length()) {
            val item = manifests.optJSONObject(i) ?: continue
            val arch = item.optJSONObject("platform")?.optString("architecture", "")
            if ((arch.isNullOrBlank() || arch == "arm64") && item.optString("digest").startsWith("sha256:")) { descriptor = item; break }
        }
        val d = descriptor ?: error("Manifest ARM64 não encontrado no Debian $suite")
        val inline = d.optString("data", "")
        val manifest = if (inline.isNotBlank()) JSONObject(String(Base64.decode(inline, Base64.DEFAULT), Charsets.UTF_8))
        else JSONObject(fetchText(base + "blobs/sha256/${d.getString("digest").removePrefix("sha256:")}"))
        val layers = manifest.optJSONArray("layers") ?: error("Manifest Debian $suite sem layers")
        var hash: String? = null
        for (i in 0 until layers.length()) {
            val l = layers.optJSONObject(i) ?: continue
            if (l.optString("digest").startsWith("sha256:") && l.optString("mediaType").contains("tar")) { hash = l.getString("digest").removePrefix("sha256:"); break }
        }
        return Rootfs(base + "blobs/rootfs.tar.gz", "debian-$suite-arm64-rootfs.tar.gz", hash, suite)
    }

    fun candidates(): List<Rootfs> {
        val r = SUITES.mapNotNull { s -> runCatching { resolve(s) }.getOrNull() }
        check(r.isNotEmpty()) { "Não foi possível resolver o rootfs Debian ARM64 (OCI). Verifique a conexão." }
        return r
    }
}
