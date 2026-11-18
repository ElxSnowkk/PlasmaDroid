
package org.plasma.app.session

import android.view.Surface
import kotlinx.coroutines.launch
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.flow.collect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.plasma.app.PlasmaApp
import org.plasma.app.ui.GlassRegistry
import org.plasma.render.gl.GlCompositor
import org.plasma.app.android.WindowLauncher
import org.plasma.bridge.AndroidApp
import org.plasma.bridge.PrivilegedBackend
import org.plasma.core.Keys
import org.plasma.core.PlasmaLog
import org.plasma.wm.Frame
import org.plasma.wm.Orientation
import org.plasma.wm.WinState
import org.plasma.wm.WindowAspect
import org.plasma.wm.WindowManagerCore

sealed interface WindowContent {
    class AndroidWin(val app: AndroidApp) : WindowContent {
        var displayId by mutableIntStateOf(-1)
        var status by mutableStateOf("Iniciando…")
        var method by mutableStateOf("")
        var interactive by mutableStateOf(false)
        var fellBack by mutableStateOf(false)
        @Volatile var backend: PrivilegedBackend? = null
        @Volatile var vd: android.hardware.display.VirtualDisplay? = null
        @Volatile var surface: Surface? = null
        @Volatile var texture: android.graphics.SurfaceTexture? = null
        @Volatile var downTime = 0L
        var w = 0
        var h = 0
    }
    data object Files : WindowContent
    data object Setup : WindowContent
    data object WaylandDisplay : WindowContent
    data class WaylandClientWindow(val waylandId: Long) : WindowContent
    data class X11ClientWindow(val xid: Long) : WindowContent
    data object Settings : WindowContent
    class Terminal(first: TerminalSession) : WindowContent {
        val tabs = androidx.compose.runtime.mutableStateListOf(first)
        var active by mutableIntStateOf(0)
    }
    class Capture : WindowContent {
        var vd: android.hardware.display.VirtualDisplay? = null
        var surface: Surface? = null
        var status by mutableStateOf("Aguardando autorização…")
    }
}

enum class TrayPopup { NONE, CALENDAR, NOTIFICATIONS, SYSTEM, NETWORK, BLUETOOTH, VOLUME, BRIGHTNESS, OVERFLOW }

class MenuItem(val label: String, val enabled: Boolean = true, val separator: Boolean = false, val icon: String? = null, val action: () -> Unit = {})
class MenuState(val x: Float, val y: Float, val items: List<MenuItem>)
data class AppImagePrompt(
    val file: java.io.File, val title: String = "", val comment: String = "", val arch: String = "", val hostArch: String = "",
    val archOk: Boolean = true, val icon: androidx.compose.ui.graphics.ImageBitmap? = null, val loading: Boolean = true,
)

class DesktopSession(val displayId: Int, val app: PlasmaApp) {

    val wm = WindowManagerCore(Frame(0f, 0f, 1280f, 720f), app.settings.int(Keys.DESKTOPS, 2))
    val contents = mutableStateMapOf<Int, WindowContent>()
    var startMenuOpen by mutableStateOf(false)
    var tray by mutableStateOf(TrayPopup.NONE)
    var locked by mutableStateOf(false)

    var appPicker by mutableStateOf(false)
    var menu by mutableStateOf<MenuState?>(null)
    var toast by mutableStateOf<String?>(null)
    var piAppsNotice by mutableStateOf(false)
    var appImagePrompt by mutableStateOf<AppImagePrompt?>(null)

    val guestIcons = mutableStateMapOf<String, androidx.compose.ui.graphics.ImageBitmap>()
    private val guestAppIdIcons = mutableMapOf<String, androidx.compose.ui.graphics.ImageBitmap>()
    var widgetPicker by mutableStateOf(false)
    var settingsPage by mutableStateOf("appearance")

    var settingsDeep by mutableStateOf(false)
    val launcher = WindowLauncher(app, this)
    val compositor = GlCompositor(app.render)
    val glass = GlassRegistry()
    val closing = mutableStateMapOf<Int, Boolean>()

    val orientOverride = mutableStateMapOf<Int, Orientation>()
    private val waylandManagedWindows = mutableMapOf<Long, Int>()
    init {

        app.appScope.launch {
            app.wayland.windows.collect { clientWindows ->
                withContext(Dispatchers.Main.immediate) {
                    if (displayId != android.view.Display.DEFAULT_DISPLAY) return@withContext

                    val liveIds = clientWindows.mapTo(mutableSetOf()) { it.id }
                    val removed = waylandManagedWindows.keys.filter { it !in liveIds }
                    removed.forEach { waylandId ->
                        val managedId = waylandManagedWindows.remove(waylandId) ?: return@forEach
                        finalizeClose(managedId)
                    }

                    clientWindows.forEachIndexed { index, client ->
                        val existing = waylandManagedWindows[client.id]
                        if (existing != null && contents.containsKey(existing)) {
                            wm.setTitle(existing, client.title.ifBlank { client.appId.ifBlank { "Aplicativo Linux" } })
                        } else {
                            val wa = wm.state.value.workArea
                            val width = (client.width.takeIf { it > 0 }?.toFloat() ?: wa.w * 0.68f)
                                .coerceIn(WindowManagerCore.MIN_W, wa.w)
                            val height = (client.height.takeIf { it > 0 }?.toFloat() ?: wa.h * 0.68f)
                                .coerceIn(WindowManagerCore.MIN_H, wa.h)
                            val offset = (index % 5) * 26f
                            val title = client.title.ifBlank { client.appId.ifBlank { "Aplicativo Linux" } }
                            val managedId = wm.open(
                                title,
                                "wayland:${client.id}",
                                "wayland-client",
                                Frame(wa.x + 24f + offset, wa.y + 24f + offset, width, height),
                            )
                            waylandManagedWindows[client.id] = managedId
                            val iconKey = "wayland:${client.id}"
                            val byAppId = client.appId.lowercase().takeIf { it.isNotBlank() }
                            (byAppId?.let { guestAppIdIcons[it] } ?: AppImageSupport.pending()?.also { ic -> byAppId?.let { guestAppIdIcons[it] = ic } })
                                ?.let { guestIcons[iconKey] = it }
                            contents[managedId] = WindowContent.WaylandClientWindow(client.id)
                        }
                    }
                }
            }
        }
    }
    var interacting by mutableStateOf(false)
    var launcherOpen by mutableStateOf(false)
    var requestProjection: () -> Unit = {}
    var pickWallpaper: () -> Unit = {}
    var pickVulkanDriver: () -> Unit = {}
    var vulkanImportStatus by mutableStateOf("")
    var vulkanImportOk by mutableStateOf(true)

    var requestPermissions: (Array<String>) -> Unit = {}

    var setWindowBrightness: (Float?) -> Unit = {}
    var frameSource: org.plasma.sharing.FrameSource? = null
    var leaveDesktop: () -> Unit = {}

    var requestStorage: () -> Unit = {}
    var openNotificationSettings: () -> Unit = {}

    fun closePopups() { startMenuOpen = false; tray = TrayPopup.NONE; menu = null; widgetPicker = false; appPicker = false }

    fun openAndroid(a: AndroidApp) {
        closePopups()
        val id = wm.open(a.label, "app:${a.pkg}", "android")
        contents[id] = WindowContent.AndroidWin(a)
    }

    fun promptAppImage(f: java.io.File) {
        closePopups()
        val fallback = AppImageSupport.fallbackIcon(app)
        appImagePrompt = AppImagePrompt(f, title = f.nameWithoutExtension, icon = fallback, loading = true)
        app.appScope.launch(Dispatchers.IO) {
            val info = runCatching { AppImageSupport.inspect(app, f) }.getOrNull()
            withContext(Dispatchers.Main) {
                val cur = appImagePrompt ?: return@withContext
                if (cur.file != f) return@withContext
                appImagePrompt = if (info == null) cur.copy(loading = false)
                else cur.copy(
                    title = info.name.ifBlank { cur.title }, comment = info.comment, arch = info.arch, hostArch = info.hostArch,
                    archOk = info.arch.isBlank() || info.arch == "desconhecida" || info.arch == info.hostArch,
                    icon = info.icon ?: fallback, loading = false,
                )
            }
        }
    }

    fun runAppImage(p: AppImagePrompt) {
        appImagePrompt = null
        AppImageSupport.markLaunch(p.icon ?: AppImageSupport.fallbackIcon(app))
        runInTerminal("/usr/local/bin/plasma-appimage run " + AppImageSupport.shq(AppImageSupport.guestPath(p.file)))
    }

    fun openLinux(a: LinuxApp) {
        closePopups()
        if (!app.debian.installed) { showToast("O Debian ainda não está instalado"); return }
        if (a.terminal) { runInTerminal(app.linuxApps.commandLine(a)); return }
        if (a.id == "pi-apps") {
            val marca = java.io.File(app.debian.paths.root, "pi-apps-aberto")
            if (!marca.exists()) {
                runCatching { marca.writeText("1\n") }
                piAppsNotice = true
            }
        }
        app.appScope.launch(Dispatchers.IO) {
            AppImageSupport.markLaunch(app.linuxApps.icon(a.id))
            runCatching { app.linuxApps.launch(a) }.onFailure {
                PlasmaLog.w("linux-app", "falha ao abrir ${a.id}", it)
                withContext(Dispatchers.Main) { showToast("Não foi possível abrir ${a.name}: ${it.message ?: it.javaClass.simpleName}") }
            }
        }
    }

    fun runInTerminal(cmd: String) {
        closePopups()
        val wa = wm.state.value.workArea
        val n = wm.state.value.windows.size % 6 * 30f
        val ts = TerminalSession(app, this)
        val id = wm.open("Konsole", "tool:terminal", "terminal", Frame(wa.x + 90f + n, wa.y + 60f + n, wa.w * 0.58f, wa.h * 0.62f))
        contents[id] = WindowContent.Terminal(ts)
        ts.runCommand(cmd)
    }

    fun openTool(tool: String, page: String? = null) {
        if (tool == "wayland") return
        closePopups()
        if (page != null) { settingsPage = page; settingsDeep = true }
        if (tool == "wayland") {

            contents.entries.firstOrNull { it.value === WindowContent.WaylandDisplay }?.let { wm.focus(it.key); return }
        }
        val existing = if (tool == "terminal") null else wm.state.value.windows.firstOrNull { it.appId == "tool:$tool" }
        if (existing != null) { wm.focus(existing.id); return }
        val wa = wm.state.value.workArea
        val (title, content) = when (tool) {
            "files" -> "Dolphin — Arquivos" to (WindowContent.Files as WindowContent)
            "settings" -> "Configurações do Sistema" to (WindowContent.Settings as WindowContent)
            "setup" -> "Startup e Configuração" to (WindowContent.Setup as WindowContent)
            "terminal" -> "Konsole" to (WindowContent.Terminal(TerminalSession(app, this)) as WindowContent)
            "wayland" -> "Wayland — Aplicativos Linux" to WindowContent.WaylandDisplay
            else -> "Captura de tela" to (WindowContent.Capture() as WindowContent)
        }
        val n = wm.state.value.windows.size % 6 * 30f

        val dens = app.resources.displayMetrics.density * org.plasma.app.ui.UiScale.effective(app, app.settings)
        val narrow = wa.w / dens < 640f
        val full = Frame(wa.x + 6f, wa.y + 6f, wa.w - 12f, wa.h - 12f)
        val size = if (narrow && tool != "capture") full else when (tool) {
            "files" -> Frame(wa.x + 40f + n, wa.y + 40f + n, wa.w * 0.62f, wa.h * 0.72f)
            "settings" -> Frame(wa.x + 60f, wa.y + 40f, wa.w * 0.8f, wa.h * 0.85f)
            "setup" -> Frame(wa.x + wa.w * 0.12f, wa.y + wa.h * 0.06f, wa.w * 0.76f, wa.h * 0.88f)
            "terminal" -> Frame(wa.x + 90f + n, wa.y + 60f + n, wa.w * 0.58f, wa.h * 0.62f)

            "wayland" -> Frame(
                wa.x + 20f + n, wa.y + 20f + n,
                (wa.w * 0.42f).coerceIn(WindowManagerCore.MIN_W, wa.w),
                (wa.h * 0.42f).coerceIn(WindowManagerCore.MIN_H, wa.h),
            )
            else -> null
        }
        val id = wm.open(title, "tool:$tool", tool, size)
        contents[id] = content
    }

    fun isAppSurface(id: Int): Boolean = when (contents[id]) {
        is WindowContent.AndroidWin, is WindowContent.WaylandClientWindow, is WindowContent.X11ClientWindow -> true
        else -> false
    }

    fun forceAspect(): Boolean = app.settings.bool(Keys.FORCE_ASPECT, true)

    fun kdeOrientation(): Orientation = WindowAspect.orientationOf(wm.state.value.screenArea)

    fun effectiveOrientation(id: Int): Orientation = orientOverride[id] ?: kdeOrientation()

    fun toggleFullscreen(id: Int) {
        val was = wm.state.value.window(id)?.state == WinState.FULLSCREEN
        closePopups()
        wm.toggleFullscreen(id)
        if (was) orientOverride.remove(id)
    }

    fun rotateWindow(id: Int) {
        val w = wm.state.value.window(id) ?: return
        if (w.state != WinState.FULLSCREEN) { wm.rotate(id); return }
        if (!forceAspect()) { showToast("Ative “Forçar proporção em tela cheia” em Configurações > Tela e monitor para girar em tela cheia"); return }
        orientOverride[id] = effectiveOrientation(id).flipped
    }

    fun closeWindow(id: Int) {
        if (closing[id] == true) return
        if (app.settings.float(Keys.FX_ANIM_SPEED, 1f) > 0f && contents.containsKey(id)) closing[id] = true else finalizeClose(id)
    }

    fun closeWindowOf(t: TerminalSession) {
        val e = contents.entries.firstOrNull { (it.value as? WindowContent.Terminal)?.tabs?.contains(t) == true } ?: return
        val c = e.value as WindowContent.Terminal
        if (c.tabs.size <= 1) { closeWindow(e.key); return }
        val i = c.tabs.indexOf(t)
        c.tabs.removeAt(i); t.close()
        c.active = c.active.coerceAtMost(c.tabs.size - 1)
    }

    private val x11ManagedWindows = mutableMapOf<Long, Int>()

    fun syncX11Windows(list: List<org.plasma.x11.X11Window>) {
        val live = list.mapTo(HashSet()) { it.id }
        for (xid in x11ManagedWindows.keys.toList()) {
            if (xid !in live) x11ManagedWindows.remove(xid)?.let { finalizeClose(it) }
        }
        list.forEachIndexed { index, win ->
            val title = win.title.ifBlank { win.cls.ifBlank { "Aplicativo X11" } }
            val existing = x11ManagedWindows[win.id]
            if (existing != null && contents.containsKey(existing)) {
                wm.setTitle(existing, title)
            } else {
                val wa = wm.state.value.workArea
                val width = (win.w.takeIf { it > 0 }?.toFloat() ?: wa.w * 0.68f).coerceIn(WindowManagerCore.MIN_W, wa.w)
                val height = (win.h.takeIf { it > 0 }?.toFloat() ?: wa.h * 0.68f).coerceIn(WindowManagerCore.MIN_H, wa.h)
                val offset = (index % 5) * 26f
                val managedId = wm.open(title, "x11:${win.id}", "x11-client",
                    Frame(wa.x + 24f + offset, wa.y + 24f + offset, width, height))
                x11ManagedWindows[win.id] = managedId
                contents[managedId] = WindowContent.X11ClientWindow(win.id)
                AppImageSupport.pending()?.let { guestIcons["x11:${win.id}"] = it }
            }
        }
    }

    fun finalizeClose(id: Int) {
        closing.remove(id)
        orientOverride.remove(id)
        when (val c = contents.remove(id)) {
            is WindowContent.Terminal -> c.tabs.forEach { it.close() }
            is WindowContent.AndroidWin -> launcher.release(c)
            is WindowContent.Capture -> { c.vd?.release(); c.surface?.release() }
            is WindowContent.WaylandClientWindow -> app.wayland.closeClientWindow(c.waylandId)
            is WindowContent.X11ClientWindow -> app.x11.closeWindow(c.xid)
            else -> {}
        }
        wm.close(id)
    }

    fun setScreen(w: Float, h: Float, panelPx: Float, panelTop: Boolean) {
        wm.setScreenArea(Frame(0f, 0f, w, h))
        wm.setWorkArea(if (panelTop) Frame(0f, panelPx, w, h - panelPx) else Frame(0f, 0f, w, h - panelPx))
    }

    fun shutdown() {
        wm.state.value.windows.map { it.id }.forEach { id ->
            val c = contents[id]
            if (c is WindowContent.WaylandClientWindow && keepAlive(c.waylandId)) return@forEach
            finalizeClose(id)
        }
    }

    private fun keepAlive(waylandId: Long): Boolean = runCatching {
        app.wayland.windows.value.any { it.id == waylandId && it.appId == "pi-apps" }
    }.getOrDefault(false)    fun showToast(t: String) { toast = t }
}

class SessionRegistry(private val app: PlasmaApp) {
    private val map = HashMap<Int, DesktopSession>()
    @Synchronized fun obtain(displayId: Int): DesktopSession = map.getOrPut(displayId) { DesktopSession(displayId, app) }
    @Synchronized fun release(displayId: Int) {
        map.remove(displayId)?.shutdown()
        if (map.isEmpty()) runCatching {
            if (app.wayland.windows.value.isEmpty()) app.wayland.stop()
        }
    }
    @Synchronized fun all(): List<DesktopSession> = map.values.toList()
}
