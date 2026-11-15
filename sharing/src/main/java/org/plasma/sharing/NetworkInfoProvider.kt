/*
 * This file belongs to the PlasmaDroid project.
 *
 * Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
 *
 * PlasmaDroid is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
 * or <https://www.gnu.org/licenses/gpl-3.0.html>.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 */

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
