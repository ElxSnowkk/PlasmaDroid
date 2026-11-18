
package org.plasma.x11

import android.view.MotionEvent
import android.view.Surface
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.json.JSONArray
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

data class X11Window(
    val id: Long,
    val title: String,
    val cls: String,
    val x: Int,
    val y: Int,
    val w: Int,
    val h: Int,
    val override: Boolean,
    val display: Int = 0,
)

class X11Server {

    private class Inst(val display: Int, val handle: Long)

    private val lock = Any()

    @Volatile
    private var insts: List<Inst> = emptyList()

    @Volatile
    private var tmpDir: File? = null

    @Volatile
    private var screenW = 1280

    @Volatile
    private var screenH = 720

    private val polling = AtomicBoolean(false)
    private val watching = AtomicBoolean(false)

    @Volatile
    private var poller: Thread? = null

    @Volatile
    private var watcher: Thread? = null

    private val _windows =
        MutableStateFlow<List<X11Window>>(emptyList())

    val windows: StateFlow<List<X11Window>> =
        _windows.asStateFlow()

    @Volatile
    var error: String? = null
        private set

    val running: Boolean
        get() = insts.isNotEmpty()

    val displays: List<Int>
        get() = insts.map { it.display }.sorted()

    val primaryDisplay: Int
        get() = insts.minOfOrNull { it.display } ?: -1

    private fun compose(display: Int, xid: Long): Long =
        (display.toLong() shl 32) or (xid and 0xFFFFFFFFL)

    private fun displayOf(id: Long): Int = (id ushr 32).toInt()

    private fun nativeId(id: Long): Int = (id and 0xFFFFFFFFL).toInt()

    private fun instOf(id: Long): Inst? {
        val d = displayOf(id)
        return insts.firstOrNull { it.display == d }
    }

    private fun startOne(
        rootTmp: File,
        display: Int,
        width: Int,
        height: Int
    ): Boolean {
        val h = runCatching {
            NativeX11Bridge.nativeStart(
                rootTmp.absolutePath,
                display,
                width,
                height
            )
        }.getOrElse {
            error = it.message ?: it.javaClass.simpleName
            0L
        }

        if (h == 0L) {
            val native = runCatching { NativeX11Bridge.nativeLastError() }
                .getOrDefault("")
            error = native.ifBlank { error ?: "Servidor X11 não iniciou." }
            return false
        }

        insts = insts + Inst(display, h)
        return true
    }

    fun start(
        rootTmp: File,
        display: Int = 0,
        width: Int = screenW,
        height: Int = screenH
    ): Boolean = synchronized(lock) {
        if (width <= 0 || height <= 0 || display < 0) {
            error = "Parâmetros inválidos para iniciar o X11."
            return false
        }

        tmpDir = rootTmp

        if (insts.any { it.display == display }) return true

        if (!startOne(rootTmp, display, width, height)) return false

        error = null
        startPolling()
        publishState(rootTmp)
        true
    }

    fun ensure(
        rootTmp: File,
        forceNew: Boolean = false,
        preferred: Int = 0,
        width: Int = screenW,
        height: Int = screenH
    ): Int = synchronized(lock) {
        tmpDir = rootTmp

        if (!forceNew) {
            val current = primaryDisplay
            if (current >= 0) return current
        }

        for (n in preferred.coerceAtLeast(0) until preferred.coerceAtLeast(0) + MAX_DISPLAYS) {
            if (insts.any { it.display == n }) continue

            if (startOne(rootTmp, n, width, height)) {
                error = null
                startPolling()
                publishState(rootTmp)
                return n
            }
        }

        error = error ?: "Nenhum display X11 livre."
        publishState(rootTmp)
        -1
    }

    fun stop(display: Int? = null) {
        synchronized(lock) {
            val targets =
                if (display == null) insts
                else insts.filter { it.display == display }

            insts = insts - targets.toSet()

            if (insts.isEmpty()) {
                polling.set(false)

                val oldPoller = poller
                poller = null

                if (oldPoller !== Thread.currentThread()) {
                    runCatching {
                        oldPoller?.join(1000)
                    }
                }
            }

            for (inst in targets) {
                runCatching {
                    NativeX11Bridge.nativeStop(inst.handle)
                }.onFailure {
                    error = it.message ?: "Erro ao finalizar servidor X11."
                }
            }

            _windows.value =
                _windows.value.filter { w -> insts.any { it.display == w.display } }

            tmpDir?.let { publishState(it) }
        }
    }

    private fun publishState(rootTmp: File) {
        runCatching {
            val file = File(rootTmp, DISPLAY_FILE)
            val primary = primaryDisplay

            if (primary < 0) {
                file.delete()
            } else {
                val tmp = File(rootTmp, "$DISPLAY_FILE.tmp")
                tmp.writeText(":$primary\n")
                tmp.renameTo(file)
            }
        }
    }

    private fun reply(rootTmp: File, text: String) {
        runCatching {
            val tmp = File(rootTmp, "$REPLY_FILE.tmp")
            tmp.writeText(text + "\n")
            tmp.renameTo(File(rootTmp, REPLY_FILE))
        }
    }

    private fun handleRequest(rootTmp: File, raw: String) {
        val parts = raw.trim().split(Regex("\\s+")).filter { it.isNotEmpty() }
        val verb = parts.firstOrNull() ?: return
        val wanted = parts.firstNotNullOfOrNull {
            if (it.startsWith(":")) it.drop(1).substringBefore('.').toIntOrNull() else null
        }

        when (verb) {
            "start" -> {
                val forceNew = parts.contains("new")

                val d = when {
                    wanted != null ->
                        if (start(rootTmp, wanted)) wanted else -1

                    else -> ensure(rootTmp, forceNew)
                }

                reply(
                    rootTmp,
                    if (d >= 0) ":$d" else "erro: ${error ?: "não foi possível iniciar"}"
                )
            }

            "stop" -> {
                stop(wanted)
                reply(rootTmp, "parado")
            }

            else -> reply(rootTmp, "erro: comando desconhecido ($verb)")
        }
    }

    fun watch(rootTmp: File) {
        synchronized(lock) {
            tmpDir = rootTmp

            if (watching.get()) return

            watching.set(true)

            runCatching { File(rootTmp, DISPLAY_FILE).delete() }
            runCatching { File(rootTmp, REPLY_FILE).delete() }

            val request = File(rootTmp, "plasma-x11.request")
            val status = File(rootTmp, "plasma-x11.status")

            watcher = Thread({
                while (watching.get() &&
                    !Thread.currentThread().isInterrupted
                ) {
                    try {
                        if (request.exists()) {
                            val cmd = request.readText().trim()
                            request.delete()
                            handleRequest(rootTmp, cmd)
                        }

                        val currentStatus =
                            if (running) "ativo " + displays.joinToString(",") { ":$it" }
                            else error ?: "inativo"

                        status.writeText("$currentStatus\n")

                        Thread.sleep(300)
                    } catch (_: InterruptedException) {
                        break
                    } catch (e: Exception) {
                        error = e.message ?: "Erro no monitor X11."
                        try {
                            Thread.sleep(500)
                        } catch (_: InterruptedException) {
                            break
                        }
                    }
                }

                watching.set(false)
            }, "plasma-x11-watch").apply {
                isDaemon = true
                start()
            }
        }
    }

    fun stopWatching() {
        watching.set(false)
        watcher?.interrupt()
        watcher = null
    }

    private fun startPolling() {
        if (!polling.compareAndSet(false, true)) return

        poller = Thread({
            try {
                while (polling.get()) {
                    val snapshot = insts
                    val merged = ArrayList<X11Window>()
                    var ok = true

                    for (inst in snapshot) {
                        runCatching {
                            parse(
                                inst.display,
                                NativeX11Bridge.nativeWindowListJson(inst.handle)
                            )
                        }.onSuccess {
                            merged += it
                        }.onFailure {
                            ok = false
                            error = it.message ?: "Erro ao consultar janelas X11."
                        }
                    }

                    if (ok && polling.get() && insts === snapshot) {
                        _windows.value = merged
                    }

                    Thread.sleep(250)
                }
            } catch (_: InterruptedException) {

            } finally {
                polling.set(false)
            }
        }, "plasma-x11-poll").apply {
            isDaemon = true
            start()
        }
    }

    private fun parse(display: Int, json: String): List<X11Window> {
        return runCatching {
            val array = JSONArray(json)

            List(array.length()) { i ->
                val obj = array.getJSONObject(i)

                X11Window(
                    id = compose(display, obj.getLong("id")),
                    title = obj.optString("title", ""),
                    cls = obj.optString("class", ""),
                    x = obj.optInt("x"),
                    y = obj.optInt("y"),
                    w = obj.optInt("w"),
                    h = obj.optInt("h"),
                    override = obj.optInt("override") != 0,
                    display = display
                )
            }
        }.getOrElse {
            error = it.message ?: "JSON de janelas X11 inválido."
            emptyList()
        }
    }

    fun windowById(id: Long): X11Window? {
        return _windows.value.firstOrNull { window ->
            window.id == id
        }
    }

    fun setScreenSize(width: Int, height: Int) {
        if (width < 64 || height < 64) return
        screenW = width
        screenH = height
        for (inst in insts) {
            runCatching { NativeX11Bridge.nativeSetScreenSize(inst.handle, width, height) }
        }
    }

    fun resizeWindow(xid: Long, width: Int, height: Int) {
        val inst = instOf(xid) ?: return
        NativeX11Bridge.nativeResizeWindow(inst.handle, nativeId(xid), width, height)
    }

    fun attachSurface(xid: Long, surface: Surface) {
        val inst = instOf(xid) ?: return
        NativeX11Bridge.nativeAttachSurface(
            inst.handle,
            nativeId(xid),
            surface
        )
    }

    fun detachSurface(xid: Long) {
        val inst = instOf(xid) ?: return
        NativeX11Bridge.nativeDetachSurface(inst.handle, nativeId(xid))
    }

    fun focus(xid: Long) {
        val inst = instOf(xid) ?: return
        NativeX11Bridge.nativeFocus(inst.handle, nativeId(xid))
    }

    fun closeWindow(xid: Long) {
        val inst = instOf(xid) ?: return
        NativeX11Bridge.nativeClose(inst.handle, nativeId(xid))
    }

    private fun buttonState(state: Int): Int {
        var result = 0

        if (state and MotionEvent.BUTTON_PRIMARY != 0)
            result = result or (1 shl 8)

        if (state and MotionEvent.BUTTON_TERTIARY != 0)
            result = result or (1 shl 9)

        if (state and MotionEvent.BUTTON_SECONDARY != 0)
            result = result or (1 shl 10)

        return result
    }

    private fun modifierState(state: Int): Int {
        var result = 0

        if (state and android.view.KeyEvent.META_SHIFT_ON != 0)
            result = result or 1

        if (state and android.view.KeyEvent.META_CTRL_ON != 0)
            result = result or 4

        if (state and android.view.KeyEvent.META_ALT_ON != 0)
            result = result or 8

        if (state and android.view.KeyEvent.META_META_ON != 0)
            result = result or 64

        return result
    }

    private fun linuxKeyCode(event: android.view.KeyEvent): Int {
        if (event.scanCode > 0) return event.scanCode
        return when (event.keyCode) {
            android.view.KeyEvent.KEYCODE_ESCAPE -> 1
            android.view.KeyEvent.KEYCODE_1 -> 2
            android.view.KeyEvent.KEYCODE_2 -> 3
            android.view.KeyEvent.KEYCODE_3 -> 4
            android.view.KeyEvent.KEYCODE_4 -> 5
            android.view.KeyEvent.KEYCODE_5 -> 6
            android.view.KeyEvent.KEYCODE_6 -> 7
            android.view.KeyEvent.KEYCODE_7 -> 8
            android.view.KeyEvent.KEYCODE_8 -> 9
            android.view.KeyEvent.KEYCODE_9 -> 10
            android.view.KeyEvent.KEYCODE_0 -> 11
            android.view.KeyEvent.KEYCODE_MINUS -> 12
            android.view.KeyEvent.KEYCODE_EQUALS -> 13
            android.view.KeyEvent.KEYCODE_DEL -> 14
            android.view.KeyEvent.KEYCODE_TAB -> 15
            android.view.KeyEvent.KEYCODE_Q -> 16
            android.view.KeyEvent.KEYCODE_W -> 17
            android.view.KeyEvent.KEYCODE_E -> 18
            android.view.KeyEvent.KEYCODE_R -> 19
            android.view.KeyEvent.KEYCODE_T -> 20
            android.view.KeyEvent.KEYCODE_Y -> 21
            android.view.KeyEvent.KEYCODE_U -> 22
            android.view.KeyEvent.KEYCODE_I -> 23
            android.view.KeyEvent.KEYCODE_O -> 24
            android.view.KeyEvent.KEYCODE_P -> 25
            android.view.KeyEvent.KEYCODE_LEFT_BRACKET -> 26
            android.view.KeyEvent.KEYCODE_RIGHT_BRACKET -> 27
            android.view.KeyEvent.KEYCODE_ENTER, android.view.KeyEvent.KEYCODE_NUMPAD_ENTER -> 28
            android.view.KeyEvent.KEYCODE_CTRL_LEFT -> 29
            android.view.KeyEvent.KEYCODE_A -> 30
            android.view.KeyEvent.KEYCODE_S -> 31
            android.view.KeyEvent.KEYCODE_D -> 32
            android.view.KeyEvent.KEYCODE_F -> 33
            android.view.KeyEvent.KEYCODE_G -> 34
            android.view.KeyEvent.KEYCODE_H -> 35
            android.view.KeyEvent.KEYCODE_J -> 36
            android.view.KeyEvent.KEYCODE_K -> 37
            android.view.KeyEvent.KEYCODE_L -> 38
            android.view.KeyEvent.KEYCODE_SEMICOLON -> 39
            android.view.KeyEvent.KEYCODE_APOSTROPHE -> 40
            android.view.KeyEvent.KEYCODE_GRAVE -> 41
            android.view.KeyEvent.KEYCODE_SHIFT_LEFT -> 42
            android.view.KeyEvent.KEYCODE_BACKSLASH -> 43
            android.view.KeyEvent.KEYCODE_Z -> 44
            android.view.KeyEvent.KEYCODE_X -> 45
            android.view.KeyEvent.KEYCODE_C -> 46
            android.view.KeyEvent.KEYCODE_V -> 47
            android.view.KeyEvent.KEYCODE_B -> 48
            android.view.KeyEvent.KEYCODE_N -> 49
            android.view.KeyEvent.KEYCODE_M -> 50
            android.view.KeyEvent.KEYCODE_COMMA -> 51
            android.view.KeyEvent.KEYCODE_PERIOD -> 52
            android.view.KeyEvent.KEYCODE_SLASH -> 53
            android.view.KeyEvent.KEYCODE_SHIFT_RIGHT -> 54
            android.view.KeyEvent.KEYCODE_ALT_LEFT -> 56
            android.view.KeyEvent.KEYCODE_SPACE -> 57
            android.view.KeyEvent.KEYCODE_CAPS_LOCK -> 58
            android.view.KeyEvent.KEYCODE_F1 -> 59
            android.view.KeyEvent.KEYCODE_F2 -> 60
            android.view.KeyEvent.KEYCODE_F3 -> 61
            android.view.KeyEvent.KEYCODE_F4 -> 62
            android.view.KeyEvent.KEYCODE_F5 -> 63
            android.view.KeyEvent.KEYCODE_F6 -> 64
            android.view.KeyEvent.KEYCODE_F7 -> 65
            android.view.KeyEvent.KEYCODE_F8 -> 66
            android.view.KeyEvent.KEYCODE_F9 -> 67
            android.view.KeyEvent.KEYCODE_F10 -> 68
            android.view.KeyEvent.KEYCODE_F11 -> 87
            android.view.KeyEvent.KEYCODE_F12 -> 88
            android.view.KeyEvent.KEYCODE_CTRL_RIGHT -> 97
            android.view.KeyEvent.KEYCODE_ALT_RIGHT -> 100
            android.view.KeyEvent.KEYCODE_MOVE_HOME, android.view.KeyEvent.KEYCODE_HOME -> 102
            android.view.KeyEvent.KEYCODE_DPAD_UP -> 103
            android.view.KeyEvent.KEYCODE_PAGE_UP -> 104
            android.view.KeyEvent.KEYCODE_DPAD_LEFT -> 105
            android.view.KeyEvent.KEYCODE_DPAD_RIGHT -> 106
            android.view.KeyEvent.KEYCODE_MOVE_END -> 107
            android.view.KeyEvent.KEYCODE_DPAD_DOWN -> 108
            android.view.KeyEvent.KEYCODE_PAGE_DOWN -> 109
            android.view.KeyEvent.KEYCODE_INSERT -> 110
            android.view.KeyEvent.KEYCODE_FORWARD_DEL -> 111
            android.view.KeyEvent.KEYCODE_META_LEFT -> 125
            android.view.KeyEvent.KEYCODE_META_RIGHT -> 126
            else -> 0
        }
    }

    fun key(xid: Long, event: android.view.KeyEvent): Boolean {
        val inst = instOf(xid) ?: return false
        val code = linuxKeyCode(event)
        if (code <= 0) return false
        val pressed = event.action != android.view.KeyEvent.ACTION_UP
        NativeX11Bridge.nativeKey(inst.handle, nativeId(xid), code, pressed, modifierState(event.metaState))
        return true
    }

    fun pointer(
        xid: Long,
        e: MotionEvent,
        viewW: Int,
        viewH: Int
    ): Boolean {
        val inst = instOf(xid) ?: return false
        if (viewW <= 0 || viewH <= 0) return false

        val h = inst.handle
        val nid = nativeId(xid)

        val win = windowById(xid) ?: return false
        if (win.w <= 0 || win.h <= 0) return false

        val index = e.actionIndex.coerceIn(0, e.pointerCount - 1)

        val x = (
            e.getX(index) * win.w / viewW
        ).toInt().coerceIn(0, win.w - 1)

        val y = (
            e.getY(index) * win.h / viewH
        ).toInt().coerceIn(0, win.h - 1)

        val state = buttonState(e.buttonState) or
            modifierState(e.metaState)

        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN,
            MotionEvent.ACTION_POINTER_DOWN -> {
                focus(xid)

                NativeX11Bridge.nativePointerMotion(
                    h, nid, x, y, state
                )

                NativeX11Bridge.nativePointerButton(
                    h, nid, x, y, 1, true, state
                )
            }

            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_UP,
            MotionEvent.ACTION_CANCEL -> {
                NativeX11Bridge.nativePointerButton(
                    h, nid, x, y, 1, false, state
                )
            }

            MotionEvent.ACTION_MOVE -> {
                NativeX11Bridge.nativePointerMotion(
                    h, nid, x, y, state
                )
            }

            else -> return false
        }

        return true
    }

    fun scroll(xid: Long, dy: Float) {
        val inst = instOf(xid) ?: return
        if (dy == 0f) return

        val button = if (dy > 0f) 4 else 5
        val nid = nativeId(xid)

        NativeX11Bridge.nativePointerButton(
            inst.handle, nid, 0, 0, button, true, 0
        )

        NativeX11Bridge.nativePointerButton(
            inst.handle, nid, 0, 0, button, false, 0
        )
    }

    private companion object {
        const val MAX_DISPLAYS = 32
        const val DISPLAY_FILE = "plasma-x11.display"
        const val REPLY_FILE = "plasma-x11.reply"
    }
}
