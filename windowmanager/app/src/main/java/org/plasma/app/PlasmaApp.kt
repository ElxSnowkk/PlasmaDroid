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

package org.plasma.app

import android.app.Application
import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.plasma.app.session.SessionRegistry
import org.plasma.app.system.SystemControls
import org.plasma.bridge.AndroidApp
import org.plasma.bridge.AppCatalog
import org.plasma.bridge.BackendManager
import org.plasma.bridge.RootBridge
import org.plasma.bridge.ShizukuBridge
import org.plasma.bridge.VirtualDisplayHost
import org.plasma.core.PlasmaLog
import org.plasma.core.SettingsStore
import org.plasma.core.SystemStats
import org.plasma.display.DisplayOutputManager
import org.plasma.proot.DebianEnvironment
import org.plasma.core.Keys
import org.plasma.proot.PlasmaVKSupport
import org.plasma.proot.ProotPaths
import org.plasma.render.RenderBackendManager
import org.plasma.sharing.SharingController
import org.plasma.app.wayland.WaylandSessionManager
import java.io.File

class PlasmaApp : Application() {
    lateinit var settings: SettingsStore
    lateinit var render: RenderBackendManager
    lateinit var shizuku: ShizukuBridge
    lateinit var root: RootBridge
    lateinit var backends: BackendManager
    lateinit var catalog: AppCatalog
    lateinit var displays: DisplayOutputManager
    lateinit var sharing: SharingController
    lateinit var stats: SystemStats
    lateinit var vdHost: VirtualDisplayHost
    lateinit var sessions: SessionRegistry
    lateinit var controls: SystemControls
    lateinit var debian: DebianEnvironment
    lateinit var wayland: WaylandSessionManager

    val appScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val _apps = MutableStateFlow<List<AndroidApp>>(emptyList())
    val apps: StateFlow<List<AndroidApp>> get() = _apps
    private val _appsReady = MutableStateFlow(false)
    val appsReady: StateFlow<Boolean> get() = _appsReady.asStateFlow()
    @Volatile var lastStopAt = 0L

    override fun onCreate() {
        super.onCreate()
        if (getProcessName() != packageName) return
        PlasmaLog.init(File(filesDir, "logs"))
        settings = SettingsStore(this)
        render = RenderBackendManager(this, settings)
        shizuku = ShizukuBridge(this)
        root = RootBridge()
        backends = BackendManager(settings, shizuku, root)
        controls = SystemControls(this)
        catalog = AppCatalog(this)
        displays = DisplayOutputManager(this)
        sharing = SharingController(this, settings)
        stats = SystemStats(this)
        vdHost = VirtualDisplayHost(this)
        debian = DebianEnvironment(this, ProotPaths(filesDir, File(applicationInfo.nativeLibraryDir)))
        wayland = WaylandSessionManager(this)
        sessions = SessionRegistry(this)
        PlasmaVKSupport.driverPreference = settings.string(Keys.VK_DRIVER, "auto").let { if (it == "injected_bionic") it else "auto" }
        PlasmaVKSupport.wsiDmabuf = settings.bool(Keys.VK_WSI_DMABUF, false)

        appScope.launch(kotlinx.coroutines.Dispatchers.IO) { runCatching { PlasmaVKSupport.initialize(this@PlasmaApp, debian.paths) }; runCatching { org.plasma.proot.SysVShm.installClient(this@PlasmaApp, debian.paths.rootfs) } }
        shizuku.start()
        displays.start()
        appScope.launch {
            try {
                _apps.value = catalog.load()
                PlasmaLog.i("app", "${_apps.value.size} apps iniciáveis")
            } finally {
                _appsReady.value = true
            }
        }
        appScope.launch { render.evaluate() }
        PlasmaLog.i("app", "KDE Plasma Android Desktop iniciado (sdk=${android.os.Build.VERSION.SDK_INT}, abi=${android.os.Build.SUPPORTED_ABIS.firstOrNull()})")
    }

    fun reloadApps() { appScope.launch { _apps.value = catalog.load() } }

    companion object { fun of(c: Context) = c.applicationContext as PlasmaApp }
}
