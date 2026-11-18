
package org.plasma.sharing

import java.net.Inet4Address
import java.net.NetworkInterface

data class NetAddr(val iface: String, val ip: String, val kind: String)

object NetworkInfoProvider {
    fun addresses(): List<NetAddr> = try {
        NetworkInterface.getNetworkInterfaces().toList().filter { it.isUp && !it.isLoopback }.flatMap { ni ->
            ni.inetAddresses.toList().filterIsInstance<Inet4Address>().map { a ->
                val n = ni.name.lowercase()
                val kind = when {
                    n.startsWith("wlan") -> "Wi-Fi"
                    n.startsWith("rndis") || n.startsWith("usb") || n.startsWith("ncm") -> "USB (tethering)"
                    n.startsWith("eth") -> "Ethernet"
                    n.startsWith("ap") || n.startsWith("swlan") -> "Hotspot"
                    else -> "Outra"
                }
                NetAddr(ni.name, a.hostAddress ?: "", kind)
            }
        }
    } catch (_: Exception) { emptyList() }
}
