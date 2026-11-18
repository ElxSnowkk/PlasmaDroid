
package org.plasma.proot

import android.content.Context
import android.net.ConnectivityManager
import java.io.File

object HostDns {
    const val GUEST_FILE = "var/lib/plasma/host-dns"

    val PUBLIC = listOf("1.1.1.1", "8.8.8.8", "9.9.9.9")

    fun deviceServers(ctx: Context): List<String> = runCatching {
        val cm = ctx.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager ?: return emptyList()
        val net = cm.activeNetwork ?: return emptyList()
        val lp = cm.getLinkProperties(net) ?: return emptyList()
        lp.dnsServers
            .mapNotNull { it.hostAddress?.substringBefore('%') }
            .filter { it.isNotBlank() && !it.startsWith("127.") && it != "::1" && !it.startsWith("0.") && !it.startsWith("fe80") }
            .distinct()
            .take(2)
    }.getOrDefault(emptyList())

    fun sync(ctx: Context, rootfs: File) {
        val servers = deviceServers(ctx)
        val hostFile = File(rootfs, GUEST_FILE)
        hostFile.parentFile?.mkdirs()
        val hostText = if (servers.isEmpty()) "" else servers.joinToString("\n", postfix = "\n")
        if (!hostFile.isFile || hostFile.readText() != hostText) hostFile.writeText(hostText)
        DebianGuest.ensureResolvConf(rootfs, servers)
    }
}
