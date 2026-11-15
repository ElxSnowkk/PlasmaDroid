// Parte do projeto PlasmaDroid.

package org.plasma.app.wayland

import android.content.Context
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.Process
import android.view.Choreographer
import android.view.KeyEvent
import android.view.Surface
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.plasma.proot.ProotPaths
import org.plasma.wayland.NativeWaylandBridge
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

class WaylandSessionManager(context: Context) {
    data class Window(
        val id: Long,
        val appId: String,
        val title: String,
        val pid: Int,
        val x: Int,
        val y: Int,
        val width: Int,
        val height: Int,
        val focused: Boolean,
        val zOrder: Int,
    ) {
        val key: String get() = id.toString()
    }

    data class RuntimeState(
        val running: Boolean = false,
        val outputAttached: Boolean = false,
        val status: String = "Wayland parado",
        val socketPath: String = "",
    )

    private val appContext = context.applicationContext
    private val dataDir = File(appContext.filesDir, "wayland")
    private val socketDir = File(dataDir, "runtime")
    private val guestRuntimePath = "/run/plasma-wayland"
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val lock = Any()

    private val renderThread = HandlerThread("plasma-wayland-render", Process.THREAD_PRIORITY_DISPLAY).apply { start() }
    private val renderHandler = Handler(renderThread.looper)

    private fun <T> onRender(default: T, block: () -> T): T {
        if (Looper.myLooper() === renderThread.looper) return block()
        val box = AtomicReference(default)
        val latch = CountDownLatch(1)
        val posted = renderHandler.post {
            try { box.set(block()) } catch (_: Throwable) {} finally { latch.countDown() }
        }
        if (!posted) return default
        return if (latch.await(3, TimeUnit.SECONDS)) box.get() else default
    }
    @Volatile private var handle: Long = 0L
    private var pollJob: kotlinx.coroutines.Job? = null
    private var lastWindowJson: String? = null
    private var lastWindowList: List<Window> = emptyList()
    @Volatile private var frameCallback: Choreographer.FrameCallback? = null
    private val framePosted = AtomicBoolean(false)
    @Volatile private var outputAttached = false
    private data class PendingClientSurface(val surface: Surface, val width: Int, val height: Int)
    private val clientSurfaces = LinkedHashMap<Long, PendingClientSurface>()
    private val attachedClientSurfaces = HashSet<Long>()
    private val suppressedClientWindows = HashSet<Long>()
    private val imeViews = LinkedHashMap<Long, org.plasma.wayland.WaylandImeInputView>()
    @Volatile private var activeImeWindowId: Long = 0L

    private inner class ImeBridge {
        fun requestSoftKeyboard(show: Boolean, windowId: Long) {
            requestSoftKeyboardForWindow(show, windowId)
        }
    }
    private val imeBridge = ImeBridge()

    private val _windows = MutableStateFlow<List<Window>>(emptyList())
    val windows: StateFlow<List<Window>> = _windows.asStateFlow()
    private val _state = MutableStateFlow(RuntimeState())
    val state: StateFlow<RuntimeState> = _state.asStateFlow()

    fun ensureStarted(paths: ProotPaths): Boolean = synchronized(lock) {
        if (handle != 0L) return@synchronized true
        socketDir.mkdirs()
        socketDir.setReadable(false, false)
        socketDir.setWritable(false, false)
        socketDir.setExecutable(false, false)
        socketDir.setReadable(true, true)
        socketDir.setWritable(true, true)
        socketDir.setExecutable(true, true)
        val xkbRoot = File(paths.rootfs, "usr/share/X11/xkb")
        val newHandle = runCatching {
            NativeWaylandBridge.nativeStartHeadless(
                socketDir.absolutePath,
                dataDir.absolutePath,
                xkbRoot.absolutePath,
                1280,
                720,
                60000,
            )
        }.getOrElse { t ->
            _state.value = RuntimeState(status = "Falha ao iniciar Wayland: ${t.message}")
            0L
        }
        if (newHandle == 0L) {
            _state.value = RuntimeState(status = "Não foi possível criar o socket Wayland")
            return@synchronized false
        }
        handle = newHandle
        runCatching { NativeWaylandBridge.nativeRegisterImeBridge(newHandle, imeBridge) }
            .onFailure { _state.value = RuntimeState(running = true, status = "Wayland ativo; IME não registrado: ${it.message}") }
        val socket = File(socketDir, "wayland-0")
        _state.value = RuntimeState(
            running = true,
            outputAttached = false,
            status = "Servidor Wayland ativo; aguardando aplicativo gráfico",
            socketPath = socket.absolutePath,
        )
        startPollingLocked()
        true
    }

    private fun startPollingLocked() {
        if (pollJob?.isActive == true) return
        pollJob = scope.launch {
            while (isActive) {
                val snapshot = synchronized(lock) {
                    val current = handle
                    if (current == 0L) return@launch

                    val json = runCatching { NativeWaylandBridge.nativeGetWindowList(current) }.getOrNull()
                    val raw = if (json != null && json == lastWindowJson) lastWindowList else runCatching {
                        parseWindows(json ?: "[]")
                    }.getOrDefault(emptyList()).also { lastWindowJson = json; lastWindowList = it }
                    val liveIds = raw.mapTo(HashSet()) { it.id }
                    suppressedClientWindows.retainAll(liveIds)
                    raw.filterNot { it.id in suppressedClientWindows }
                }
                _windows.value = snapshot
                val old = _state.value
                _state.value = old.copy(
                    status = if (old.running) "Wayland em execução" else old.status
                )
                delay(if (snapshot.isEmpty()) 1000L else 350L)
            }
        }
    }

    private fun parseWindows(json: String): List<Window> {
        val array = JSONArray(json)
        return buildList(array.length()) {
            for (i in 0 until array.length()) {
                val o = array.optJSONObject(i) ?: continue
                add(Window(
                    id = o.optLong("id"),
                    appId = o.optString("app_id"),
                    title = o.optString("title").ifBlank { o.optString("app_id", "Aplicativo Linux") },
                    pid = o.optInt("pid"),
                    x = o.optInt("x"),
                    y = o.optInt("y"),
                    width = o.optInt("w"),
                    height = o.optInt("h"),
                    focused = o.optBoolean("focused"),
                    zOrder = o.optInt("z_order"),
                ))
            }
        }
    }

    fun clientEnvironment(x11: Boolean = false): Map<String, String> =
        if (x11) mapOf(
            "XDG_RUNTIME_DIR" to guestRuntimePath,
            "WAYLAND_DISPLAY" to "wayland-0",
            "DISPLAY" to ":0",
            "XDG_SESSION_TYPE" to "x11",
            "GDK_BACKEND" to "x11",
            "QT_QPA_PLATFORM" to "xcb",
            "SDL_VIDEODRIVER" to "x11",
            "MOZ_ENABLE_WAYLAND" to "0",
            "PLASMA_X11" to "1",
        ) else mapOf(
            "XDG_RUNTIME_DIR" to guestRuntimePath,
            "XDG_SESSION_TYPE" to "wayland",
            "WAYLAND_DISPLAY" to "wayland-0",
            "GDK_BACKEND" to "wayland",
            "QT_QPA_PLATFORM" to "wayland",
            "SDL_VIDEODRIVER" to "wayland",
            "MOZ_ENABLE_WAYLAND" to "1",
            "EGL_PLATFORM" to "wayland",
        )

    fun clientBinds(paths: ProotPaths): List<Pair<File, String>> {
        File(paths.rootfs, guestRuntimePath.removePrefix("/")).mkdirs()
        return listOf(socketDir to guestRuntimePath)
    }

    fun attachSurface(surface: Surface, width: Int, height: Int): Boolean {
        if (!surface.isValid) return false
        val attached = synchronized(lock) {
            val current = handle
            if (current == 0L) return@synchronized false
            val ok = runCatching { onRender(false) { NativeWaylandBridge.nativeAttachSurface(current, surface) } }.getOrDefault(false)
            if (ok && width > 0 && height > 0)
                runCatching { onRender(Unit) { NativeWaylandBridge.nativeResize(current, width, height) } }
            ok
        }
        if (attached) {
            outputAttached = true
            _state.value = _state.value.copy(outputAttached = true)
            synchronized(lock) {

                attachedClientSurfaces.clear()
                attachPendingClientSurfacesLocked()
            }
            startFrameLoop()
        }
        return attached
    }

    private fun attachPendingClientSurfacesLocked() {
        val current = handle
        if (current == 0L || !outputAttached) return
        clientSurfaces.forEach { (id, item) ->
            if (id in attachedClientSurfaces || !item.surface.isValid) return@forEach
            val ok = runCatching {
                onRender(false) { NativeWaylandBridge.nativeAttachClientWindow(current, id, item.surface, item.width, item.height) }
            }.getOrDefault(false)
            if (ok) attachedClientSurfaces.add(id)
        }
    }

    fun attachClientSurface(windowId: Long, surface: Surface, width: Int, height: Int): Boolean {
        if (windowId <= 0 || !surface.isValid) return false
        return synchronized(lock) {
            val old = clientSurfaces.put(windowId, PendingClientSurface(surface, width, height))
            val current = handle
            if (old != null && old.surface !== surface && attachedClientSurfaces.remove(windowId) && current != 0L) {
                runCatching { onRender(Unit) { NativeWaylandBridge.nativeDetachClientWindow(current, windowId) } }
            }
            if (current == 0L || !outputAttached) return@synchronized true
            val ok = runCatching {
                onRender(false) { NativeWaylandBridge.nativeAttachClientWindow(current, windowId, surface, width, height) }
            }.getOrDefault(false)
            if (ok) attachedClientSurfaces.add(windowId)
            ok
        }
    }

    fun detachClientSurface(windowId: Long) {
        synchronized(lock) {
            clientSurfaces.remove(windowId)
            val current = handle
            if (attachedClientSurfaces.remove(windowId) && current != 0L)
                runCatching { onRender(Unit) { NativeWaylandBridge.nativeDetachClientWindow(current, windowId) } }
        }
        synchronized(imeViews) { imeViews.remove(windowId)?.let { it.clearFocus() } }
    }

    fun closeClientWindow(windowId: Long) {
        if (windowId <= 0) return
        synchronized(lock) {
            suppressedClientWindows.add(windowId)
            val current = handle
            if (current != 0L) runCatching { NativeWaylandBridge.nativeCloseToplevel(current, windowId) }
            clientSurfaces.remove(windowId)
            if (attachedClientSurfaces.remove(windowId) && current != 0L)
                runCatching { onRender(Unit) { NativeWaylandBridge.nativeDetachClientWindow(current, windowId) } }
        }
        synchronized(imeViews) { imeViews.remove(windowId)?.let { it.clearFocus() } }
        _windows.value = _windows.value.filterNot { it.id == windowId }
    }

    fun resizeClientSurface(windowId: Long, width: Int, height: Int) {
        if (width <= 0 || height <= 0) return
        synchronized(lock) {
            clientSurfaces[windowId]?.let { clientSurfaces[windowId] = it.copy(width = width, height = height) }
            val current = handle
            if (current != 0L && windowId in attachedClientSurfaces)
                runCatching { onRender(Unit) { NativeWaylandBridge.nativeResizeClientWindow(current, windowId, width, height) } }
        }
    }

    fun registerImeView(windowId: Long, view: org.plasma.wayland.WaylandImeInputView) {
        synchronized(imeViews) { imeViews[windowId] = view }
    }

    fun unregisterImeView(windowId: Long, view: org.plasma.wayland.WaylandImeInputView) {
        synchronized(imeViews) {
            if (imeViews[windowId] === view) imeViews.remove(windowId)
        }
    }

    private fun requestSoftKeyboardForWindow(show: Boolean, windowId: Long) {
        if (windowId > 0) activeImeWindowId = windowId
        val view = synchronized(imeViews) {
            val requested = if (windowId > 0) imeViews[windowId] else null
            requested ?: imeViews[activeImeWindowId] ?: imeViews.values.lastOrNull()
        } ?: return
        view.requestSoftKeyboard(show)
    }

    fun commitImeText(text: String) {
        synchronized(lock) {
            val current = handle
            if (current != 0L && text.isNotEmpty()) runCatching { NativeWaylandBridge.nativeImeCommitText(current, text) }
        }
    }

    fun setImePreedit(text: String) {
        synchronized(lock) {
            val current = handle
            if (current != 0L) runCatching {
                val utf8Cursor = text.toByteArray(Charsets.UTF_8).size
                NativeWaylandBridge.nativeImeSetPreeditText(current, text, utf8Cursor, utf8Cursor)
            }
        }
    }

    fun resizeSurface(width: Int, height: Int) {
        if (width <= 0 || height <= 0 || !outputAttached) return
        synchronized(lock) {
            val current = handle
            if (current != 0L) runCatching { onRender(Unit) { NativeWaylandBridge.nativeResize(current, width, height) } }
        }
    }

    fun detachSurface() {
        stopFrameLoop()
        val detached = synchronized(lock) {
            val current = handle
            if (current != 0L && outputAttached) {
                runCatching { onRender(Unit) { NativeWaylandBridge.nativeDetachSurface(current) } }
                true
            } else false
        }
        if (detached) {
            outputAttached = false
            synchronized(lock) { attachedClientSurfaces.clear() }
            _state.value = _state.value.copy(outputAttached = false)
        }
    }

    private fun startFrameLoop() {
        if (!framePosted.compareAndSet(false, true)) return
        val callback = object : Choreographer.FrameCallback {
            override fun doFrame(frameTimeNanos: Long) {
                if (frameCallback !== this) return
                val current = handle
                if (!framePosted.get() || current == 0L || !outputAttached) {
                    framePosted.set(false)
                    frameCallback = null
                    return
                }
                runCatching { NativeWaylandBridge.nativeRenderFrame(current, frameTimeNanos) }
                Choreographer.getInstance().postFrameCallback(this)
            }
        }
        frameCallback = callback
        renderHandler.post { Choreographer.getInstance().postFrameCallback(callback) }
    }

    private fun stopFrameLoop() {
        framePosted.set(false)
        val callback = frameCallback
        frameCallback = null
        if (callback != null) renderHandler.post { runCatching { Choreographer.getInstance().removeFrameCallback(callback) } }
    }

    fun dispatchMotion(source: Int, action: Int, pointerId: Int, x: Float, y: Float, buttons: Int, eventTimeNs: Long) {
        synchronized(lock) {
            val current = handle
            if (current != 0L && outputAttached) runCatching {
                NativeWaylandBridge.nativeDispatchMotion(current, source, action, pointerId, x, y, buttons, eventTimeNs)
            }
        }
    }

    fun focusClientWindow(windowId: Long) {
        activeImeWindowId = windowId
        synchronized(lock) {
            val current = handle
            if (current != 0L) runCatching { NativeWaylandBridge.nativeFocusToplevel(current, windowId) }
        }
    }

    fun dispatchClientMotion(windowId: Long, source: Int, action: Int, pointerId: Int,
                             x: Float, y: Float, buttons: Int, eventTimeNs: Long) {
        if (action == 0 || action == 5 || action == 7 || action == 9) focusClientWindow(windowId)
        synchronized(lock) {
            val current = handle
            if (current != 0L && outputAttached) runCatching {
                NativeWaylandBridge.nativeDispatchMotion(current, source, action, pointerId, x, y, buttons, eventTimeNs)
            }
        }
    }

    private fun linuxKeyCode(event: KeyEvent): Int {
        if (event.scanCode > 0) return event.scanCode
        return when (event.keyCode) {
            KeyEvent.KEYCODE_DEL -> 14
            KeyEvent.KEYCODE_ENTER -> 28
            KeyEvent.KEYCODE_TAB -> 15
            KeyEvent.KEYCODE_SPACE -> 57
            KeyEvent.KEYCODE_ESCAPE -> 1
            KeyEvent.KEYCODE_FORWARD_DEL -> 111
            KeyEvent.KEYCODE_DPAD_UP -> 103
            KeyEvent.KEYCODE_DPAD_DOWN -> 108
            KeyEvent.KEYCODE_DPAD_LEFT -> 105
            KeyEvent.KEYCODE_DPAD_RIGHT -> 106
            KeyEvent.KEYCODE_MOVE_HOME, KeyEvent.KEYCODE_HOME -> 102
            KeyEvent.KEYCODE_MOVE_END -> 107
            KeyEvent.KEYCODE_PAGE_UP -> 104
            KeyEvent.KEYCODE_PAGE_DOWN -> 109
            KeyEvent.KEYCODE_SHIFT_LEFT -> 42
            KeyEvent.KEYCODE_SHIFT_RIGHT -> 54
            KeyEvent.KEYCODE_CTRL_LEFT -> 29
            KeyEvent.KEYCODE_CTRL_RIGHT -> 97
            KeyEvent.KEYCODE_ALT_LEFT -> 56
            KeyEvent.KEYCODE_ALT_RIGHT -> 100
            else -> 0
        }
    }

    fun dispatchKey(event: KeyEvent) {
        if (event.action != KeyEvent.ACTION_DOWN && event.action != KeyEvent.ACTION_UP) return
        val linuxCode = linuxKeyCode(event)
        if (linuxCode <= 0) return
        synchronized(lock) {
            val current = handle
            if (current != 0L && outputAttached) runCatching {
                NativeWaylandBridge.nativeDispatchKey(current, linuxCode, event.action, System.nanoTime(), event.metaState)
            }
        }
    }

    fun dispatchScroll(dx: Float, dy: Float, eventTimeNs: Long) {
        synchronized(lock) {
            val current = handle
            if (current != 0L && outputAttached) runCatching {
                NativeWaylandBridge.nativeDispatchScroll(current, dx, dy, eventTimeNs)
            }
        }
    }

    fun stop() {
        stopFrameLoop()
        synchronized(lock) {
            val current = handle
            handle = 0L
            outputAttached = false
            if (current != 0L) {
                runCatching { NativeWaylandBridge.nativeUnregisterImeBridge(current) }
                runCatching { onRender(Unit) { NativeWaylandBridge.nativeStop(current) } }
            }
            clientSurfaces.clear()
            attachedClientSurfaces.clear()
            suppressedClientWindows.clear()
            synchronized(imeViews) { imeViews.values.forEach { it.clearFocus() }; imeViews.clear() }
            activeImeWindowId = 0L
            pollJob?.cancel()
            pollJob = null
            _windows.value = emptyList()
            _state.value = RuntimeState()
        }
    }

    fun close() {
        stop()
        scope.cancel()
        renderThread.quitSafely()
    }
}
