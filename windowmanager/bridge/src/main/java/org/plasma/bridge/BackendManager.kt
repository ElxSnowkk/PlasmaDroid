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

import org.plasma.core.Keys
import org.plasma.core.SettingsStore

class BackendManager(
    private val settings: SettingsStore,
    val shizuku: ShizukuBridge,
    val root: RootBridge,
) {
    fun current(): PrivilegedBackend? =
        shizuku.backend() ?: if (settings.bool(Keys.ROOT_ON) && root.available.value == true) root.backend else null

    fun setForceResizable(on: Boolean): String? {
        val b = current() ?: return "Sem Shizuku/root ativos"
        val out = b.exec(listOf("settings", "put", "global", "force_resizable_activities", if (on) "1" else "0"))
        return if (out.contains("exit=0") || !out.contains("Exception")) null else out.take(200)
    }
}
