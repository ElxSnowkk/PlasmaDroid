
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
