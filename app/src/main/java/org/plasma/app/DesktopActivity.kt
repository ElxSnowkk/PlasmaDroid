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

import androidx.compose.runtime.setValue
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import android.Manifest
import android.content.Intent
import android.graphics.Canvas
import android.graphics.Color
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.os.SystemClock
import android.view.KeyEvent
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import org.plasma.proot.PlasmaVKSupport
import java.io.File
import org.plasma.app.session.DesktopSession
import org.plasma.app.session.TrayPopup
import org.plasma.app.session.WindowContent
import org.plasma.app.ui.DesktopRoot
import org.plasma.app.ui.PlasmaTheme
import org.plasma.app.ui.ShortcutHandler
import org.plasma.bridge.ProjectionService
import org.plasma.core.Keys
import org.plasma.core.PlasmaLog
import org.plasma.sharing.FrameSource
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.min

class DesktopActivity : ComponentActivity() {
    private lateinit var app: PlasmaApp
    private lateinit var session: DesktopSession
    private lateinit var shortcuts: ShortcutHandler
    private var did = 0
    private val dirty = AtomicBoolean(true)

    private val projectionLauncher = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { r ->
        val data = r.data
        if (r.resultCode == RESULT_OK && data != null)
            startForegroundService(Intent(this, ProjectionService::class.java).putExtra("code", r.resultCode).putExtra("data", data))
        else session.showToast("Captura de tela não autorizada")
    }
    private val wallpaperPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            runCatching { contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION) }
            app.settings.put(Keys.WALLPAPER, uri.toString())
        }
    }
    private val vulkanDriverPicker = registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (!uris.isNullOrEmpty()) app.appScope.launch(Dispatchers.IO) { importVulkanDriver(uris) }
    }
    private val storagePermissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {}
    private val runtimePermissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { app.controls.bump() }

    private suspend fun importVulkanDriver(uris: List<Uri>) {
        if (!app.debian.installed) {
            val msg = "Instale o ambiente Debian antes de importar o driver (Configurações ou assistente)."
            runOnUiThread { session.vulkanImportOk = false; session.vulkanImportStatus = msg; session.showToast(msg) }
            return
        }
        val tmp = File(cacheDir, "vk-inject").apply { deleteRecursively(); mkdirs() }
        fun safe(n: String) = n.substringAfterLast('/').replace(Regex("[^A-Za-z0-9._+-]"), "_")
        fun isLib(n: String) = n.endsWith(".so") || n.contains(".so.")
        val files = ArrayList<File>()
        for (uri in uris) {
            val name = contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                ?.use { c -> if (c.moveToFirst()) c.getString(0) else null }
                ?: uri.lastPathSegment?.substringAfterLast('/') ?: continue
            if (name.endsWith(".zip", ignoreCase = true)) {
                runCatching {
                    contentResolver.openInputStream(uri)?.use { raw ->
                        java.util.zip.ZipInputStream(raw.buffered()).use { zin ->
                            var total = 0L
                            while (true) {
                                val e = zin.nextEntry ?: break
                                if (e.isDirectory) continue
                                val n = safe(e.name)
                                if (n.isEmpty() || !isLib(n)) continue
                                val dest = File(tmp, n)
                                dest.outputStream().use { o -> total += zin.copyTo(o) }
                                if (total > 1L shl 30) error("zip grande demais")
                                files += dest
                            }
                        }
                    }
                }.onFailure { PlasmaLog.w("vk", "falha lendo zip $name", it) }
            } else {
                val dest = File(tmp, safe(name))
                if (runCatching { contentResolver.openInputStream(uri)?.use { i -> dest.outputStream().use { o -> i.copyTo(o) } } }.getOrNull() != null)
                    if (isLib(dest.name)) files += dest
            }
        }
        val r = if (files.isEmpty()) PlasmaVKSupport.InjectResult(false, "Nenhum .so encontrado. Escolha um .so ou um .zip com o driver Vulkan Bionic.")
        else PlasmaVKSupport.installInjectedBionic(app.debian.paths.rootfs, files)
        val names = files.joinToString(", ") { it.name }
        tmp.deleteRecursively()
        runOnUiThread { session.vulkanImportOk = r.ok; session.vulkanImportStatus = r.message; session.showToast(r.message) }
        if (r.ok) {
            app.settings.put(Keys.VK_DRIVER, "injected_bionic")
            PlasmaVKSupport.driverPreference = "injected_bionic"
            app.settings.put(Keys.VK_INJECTED_BIONIC, names)
            app.debian.recheckVulkan()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        app = PlasmaApp.of(this)
        @Suppress("DEPRECATION")
        did = windowManager.defaultDisplay.displayId
        session = app.sessions.obtain(did)
        shortcuts = ShortcutHandler(session, this)
        PlasmaLog.i("activity", "desktop criado no display $did")

        session.requestProjection = {
            projectionLauncher.launch((getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager).createScreenCaptureIntent())
        }
        session.pickWallpaper = { wallpaperPicker.launch(arrayOf("image/*")) }
        session.pickVulkanDriver = { vulkanDriverPicker.launch(arrayOf("*/*")) }
        session.requestPermissions = { perms -> runCatching { runtimePermissions.launch(perms) } }
        session.setWindowBrightness = { f ->
            runOnUiThread {
                window.attributes = window.attributes.also {
                    it.screenBrightness = f ?: WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
                }
            }
        }
        session.leaveDesktop = { finish() }
        session.frameSource = object : FrameSource {
            override fun draw(canvas: Canvas, w: Int, h: Int) {
                val root = window.decorView
                if (root.width == 0 || root.height == 0) return
                canvas.drawColor(Color.BLACK)
                val s = min(w / root.width.toFloat(), h / root.height.toFloat())
                canvas.save()
                canvas.translate((w - root.width * s) / 2f, (h - root.height * s) / 2f)
                canvas.scale(s, s)
                root.draw(canvas)
                canvas.restore()
            }
            override fun consumeDirty() = dirty.getAndSet(false)
        }

        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            hide(WindowInsetsCompat.Type.systemBars())
        }
        window.decorView.viewTreeObserver.addOnDrawListener { dirty.set(true) }

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                when {
                    session.locked -> session.locked = false
                    session.startMenuOpen || session.tray != TrayPopup.NONE || session.menu != null -> session.closePopups()
                    else -> {
                        val c = focusedAndroid()
                        val be = c?.backend
                        if (c != null && be != null && c.interactive) {
                            val t = SystemClock.uptimeMillis()
                            be.key(c.displayId, KeyEvent.KEYCODE_BACK, KeyEvent.ACTION_DOWN, 0, t, t)
                            be.key(c.displayId, KeyEvent.KEYCODE_BACK, KeyEvent.ACTION_UP, 0, t, t)
                        } else session.showToast("Use Menu > Sair para fechar o desktop")
                    }
                }
            }
        })

        setContent {
            val gpu by session.compositor.info.collectAsState()
            PlasmaTheme(app.settings, gpuAlive = gpu.ready) {
                org.plasma.app.ui.StartupGate(
                    session = session,
                    gpuReady = gpu.ready,
                    onRequestStorage = {
                        if (Build.VERSION.SDK_INT >= 30) {
                            runCatching {
                                startActivity(
                                    Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                        android.net.Uri.parse("package:$packageName"))
                                )
                            }
                        } else {
                            storagePermissions.launch(arrayOf(
                                Manifest.permission.READ_EXTERNAL_STORAGE,
                                Manifest.permission.WRITE_EXTERNAL_STORAGE,
                            ))
                        }
                    },
                    onOpenNotificationSettings = {
                        runCatching {
                            startActivity(Intent(android.provider.Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
                        }
                    },
                )
            }
        }

        val autoLaunched = HashSet<Int>()
        lifecycleScope.launch {
            app.displays.displays.collect { list ->
                autoLaunched.retainAll(list.map { it.id }.toSet())
                if (did == android.view.Display.DEFAULT_DISPLAY && app.settings.bool(Keys.AUTO_EXTERNAL, false)) {
                    list.filter { it.isExternal }.forEach { d ->
                        if (app.sessions.all().none { it.displayId == d.id } && autoLaunched.add(d.id))
                            app.displays.launchOn(Intent(this@DesktopActivity, DesktopActivity::class.java), d.id)
                    }
                }
            }
        }
        lifecycleScope.launch {
            app.sharing.stats.collect { s ->
                if (s.running) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            }
        }
        lifecycleScope.launch { app.settings.version.collect { applyDisplayMode() } }
    }

    private fun applyDisplayMode() {
        val mode = app.settings.int(Keys.displayMode(did), -1)
        if (mode > 0 && window.attributes.preferredDisplayModeId != mode)
            window.attributes = window.attributes.also { it.preferredDisplayModeId = mode }
    }

    private fun focusedAndroid(): WindowContent.AndroidWin? =
        session.wm.state.value.focusedId?.let { session.contents[it] } as? WindowContent.AndroidWin

    override fun dispatchKeyEvent(e: KeyEvent): Boolean {
        if (e.action == KeyEvent.ACTION_DOWN && e.repeatCount == 0) {
            val focusedId = session.wm.state.value.focusedId
            val focusedWindow = focusedId?.let { session.wm.state.value.window(it) }
            if (e.keyCode == KeyEvent.KEYCODE_F11 && focusedId != null) {
                session.toggleFullscreen(focusedId)
                return true
            }
            if (e.keyCode == KeyEvent.KEYCODE_ESCAPE && focusedId != null && focusedWindow?.state == org.plasma.wm.WinState.FULLSCREEN) {
                session.toggleFullscreen(focusedId)
                return true
            }
        }
        if (shortcuts.handle(e)) return true
        val focusedWaylandContent = session.wm.state.value.focusedId?.let { session.contents[it] }
        val focusedForWayland = focusedWaylandContent === WindowContent.WaylandDisplay ||
            focusedWaylandContent is WindowContent.WaylandClientWindow
        if (focusedForWayland && e.keyCode != KeyEvent.KEYCODE_BACK) {

            (focusedWaylandContent as? WindowContent.WaylandClientWindow)?.let {
                app.wayland.focusClientWindow(it.waylandId)
            }
            app.wayland.dispatchKey(e)
            return true
        }
        if (e.keyCode != KeyEvent.KEYCODE_BACK && !session.startMenuOpen && session.tray == TrayPopup.NONE && !session.locked) {
            val c = focusedAndroid()
            val be = c?.backend
            val sys = e.keyCode == KeyEvent.KEYCODE_VOLUME_UP || e.keyCode == KeyEvent.KEYCODE_VOLUME_DOWN || e.keyCode == KeyEvent.KEYCODE_POWER
            if (c != null && be != null && c.interactive && c.displayId > 0 && !sys) {
                be.key(c.displayId, e.keyCode, e.action, e.metaState, e.downTime, e.eventTime)
                return true
            }
        }
        return super.dispatchKeyEvent(e)
    }

    override fun onResume() { super.onResume(); applyDisplayMode(); app.shizuku.refresh() }
    override fun onStop() { super.onStop(); app.lastStopAt = SystemClock.uptimeMillis() }

    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing) {
            if (did == android.view.Display.DEFAULT_DISPLAY) app.sharing.stop()
            app.sessions.release(did)
            PlasmaLog.i("activity", "desktop do display $did encerrado")
        }
    }
}
