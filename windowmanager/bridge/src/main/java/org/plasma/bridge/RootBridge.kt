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

package org.plasma.bridge

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import org.plasma.core.PlasmaLog
import java.util.concurrent.TimeUnit

class RootBridge {
    private val _available = MutableStateFlow<Boolean?>(null)
    val available: StateFlow<Boolean?> = _available.asStateFlow()
    val backend: PrivilegedBackend = RootBackend { cmd, t -> su(cmd, t) }

    suspend fun detect(): Boolean = withContext(Dispatchers.IO) {
        val out = su(listOf("id"), 8000)
        val ok = out.contains("uid=0")
        PlasmaLog.i("root", "deteccao: ${if (ok) "root disponivel" else "sem root"} (${out.take(80).replace('\n', ' ')})")
        _available.value = ok
        ok
    }

    private fun su(cmd: List<String>, timeoutMs: Int): String = try {
        val p = ProcessBuilder("su", "-c", cmd.joinToString(" ") { "'" + it.replace("'", "'\\''") + "'" })
            .redirectErrorStream(true).start()
        if (!p.waitFor(timeoutMs.toLong(), TimeUnit.MILLISECONDS)) { p.destroy(); "timeout" }
        else p.inputStream.readBytes().decodeToString()
    } catch (t: Throwable) { "erro: $t" }
}
