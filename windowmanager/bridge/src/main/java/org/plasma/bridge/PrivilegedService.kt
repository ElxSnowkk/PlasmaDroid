
package org.plasma.bridge

import android.annotation.TargetApi
import android.content.Context
import android.content.ContextWrapper
import android.content.AttributionSource
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothProfile
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.view.InputDevice
import android.view.InputEvent
import android.view.KeyEvent
import android.view.MotionEvent
import org.plasma.core.PlasmaLog
import android.view.Surface
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeoutException
import java.util.concurrent.TimeUnit

class PrivilegedService : IPrivilegedService.Stub() {
    private val displays = HashMap<Int, VirtualDisplay>()
    @Volatile private var err = ""
    private val worker = HandlerThread("plasma-display").also { it.start() }
    private val handler = Handler(worker.looper)
    private var fake: Context? = null

    override fun destroy() {
        runCatching { displays.values.forEach { it.release() }; displays.clear() }
        System.exit(0)
    }

    override fun uid(): Int = Process.myUid()
    override fun lastError(): String = err

    private class FakeContext(base: Context) : ContextWrapper(base) {
        override fun getPackageName() = "com.android.shell"
        override fun getOpPackageName() = "com.android.shell"
        override fun getApplicationContext(): Context = this
        @TargetApi(31)
        override fun getAttributionSource(): AttributionSource =
            AttributionSource.Builder(2000).setPackageName("com.android.shell").build()
    }

    private fun shellContext(): Context {
        fake?.let { return it }
        val at = Class.forName("android.app.ActivityThread")
        val ctor = at.getDeclaredConstructor().also { it.isAccessible = true }
        val inst = ctor.newInstance()
        at.getDeclaredField("sCurrentActivityThread").apply { isAccessible = true; set(null, inst) }
        at.getDeclaredField("mSystemThread").apply { isAccessible = true; setBoolean(inst, true) }
        val sys = at.getDeclaredMethod("getSystemContext").invoke(inst) as Context
        return FakeContext(sys).also { fake = it }
    }

    private fun <T> onWorker(timeoutMs: Long = 6000, block: () -> T): T? {
        var out: T? = null
        var failure: Throwable? = null
        val latch = CountDownLatch(1)
        handler.post {
            try { out = block() } catch (t: Throwable) { failure = t } finally { latch.countDown() }
        }
        if (!latch.await(timeoutMs, TimeUnit.MILLISECONDS)) { err = "timeout"; return null }
        failure?.let { err = "${it.javaClass.simpleName}: ${it.message}"; return null }
        return out
    }

    override fun createDisplay(name: String, width: Int, height: Int, dpi: Int, surface: Surface, flags: Int): Int {
        err = ""
        return onWorker {
            val ctor = DisplayManager::class.java.getDeclaredConstructor(Context::class.java).also { it.isAccessible = true }
            val dm = ctor.newInstance(shellContext())
            val vd = dm.createVirtualDisplay(name, width, height, dpi, surface, flags)
                ?: error("createVirtualDisplay retornou null")
            val id = vd.display.displayId
            displays[id] = vd
            id
        } ?: -1
    }

    override fun releaseDisplay(displayId: Int) {
        displays.remove(displayId)?.let { runCatching { it.release() } }
        inputHandler.post { streams.keys.removeAll { (it shr 32).toInt() == displayId } }
    }
    override fun resizeDisplay(displayId: Int, width: Int, height: Int, dpi: Int) { displays[displayId]?.resize(width, height, dpi) }
    override fun setDisplaySurface(displayId: Int, surface: Surface) { displays[displayId]?.surface = surface }

    override fun exec(cmd: Array<String>, timeoutMs: Int): String = try {
        val p = ProcessBuilder(*cmd).redirectErrorStream(true).start()
        if (!p.waitFor(timeoutMs.toLong(), TimeUnit.MILLISECONDS)) { p.destroy(); "exit=timeout" }
        else "exit=${p.exitValue()}\n" + p.inputStream.readBytes().decodeToString()
    } catch (t: Throwable) { err = t.toString(); "exit=error\n$t" }

    override fun bluetooth(op: String, address: String, timeoutMs: Int): String {
        err = ""
        return try {
            @Suppress("DEPRECATION")
            val adapter = BluetoothAdapter.getDefaultAdapter() ?: return "erro: sem adaptador Bluetooth"
            val dev = adapter.getRemoteDevice(address)
            when (op) {
                "unpair" -> {
                    val m = BluetoothDevice::class.java.getMethod("removeBond")
                    if (m.invoke(dev) as? Boolean == true) "ok" else "erro: removeBond recusado"
                }
                "connect", "disconnect" -> profileCall(adapter, dev, op == "connect", timeoutMs)
                else -> "erro: operação desconhecida: $op"
            }
        } catch (t: Throwable) {
            err = t.toString()
            "erro: ${t.javaClass.simpleName}: ${t.cause?.message ?: t.message}"
        }
    }

    private fun profileCall(adapter: BluetoothAdapter, dev: BluetoothDevice, connect: Boolean, timeoutMs: Int): String {
        val ctx = shellContext()
        val notes = ArrayList<String>()
        var anyOk = false
        for (profile in intArrayOf(BluetoothProfile.A2DP, BluetoothProfile.HEADSET)) {
            val latch = CountDownLatch(1)
            var proxy: BluetoothProfile? = null
            val listener = object : BluetoothProfile.ServiceListener {
                override fun onServiceConnected(p: Int, b: BluetoothProfile) { proxy = b; latch.countDown() }
                override fun onServiceDisconnected(p: Int) {}
            }
            if (!adapter.getProfileProxy(ctx, listener, profile)) { notes += "perfil $profile indisponível"; continue }
            if (!latch.await(timeoutMs.toLong(), TimeUnit.MILLISECONDS)) { notes += "perfil $profile: timeout"; continue }
            val px = proxy ?: continue
            try {
                val m = px.javaClass.getMethod(if (connect) "connect" else "disconnect", BluetoothDevice::class.java)
                m.isAccessible = true
                if (m.invoke(px, dev) as? Boolean == true) anyOk = true else notes += "perfil $profile: recusado"
            } catch (t: Throwable) {
                notes += "perfil $profile: ${t.cause?.message ?: t.message}"
            } finally {
                runCatching { adapter.closeProfileProxy(profile, px) }
            }
        }
        return if (anyOk) "ok" else "erro: " + notes.joinToString("; ")
    }

    private val inputManager: Any by lazy {
        Class.forName("android.hardware.input.InputManager").getDeclaredMethod("getInstance").invoke(null)!!
    }
    private val injectMethod by lazy {
        Class.forName("android.hardware.input.InputManager")
            .getMethod("injectInputEvent", InputEvent::class.java, Int::class.javaPrimitiveType)
    }
    private val setDisplayIdMethod by lazy { InputEvent::class.java.getMethod("setDisplayId", Int::class.javaPrimitiveType) }

    private fun inject(ev: InputEvent, displayId: Int, @Suppress("UNUSED_PARAMETER") discrete: Boolean = false): Boolean {
        return try {
            setDisplayIdMethod.invoke(ev, displayId)
            val accepted = injectMethod.invoke(inputManager, ev, 0) as? Boolean ?: false
            err = if (accepted) "" else "inject: InputManager recusou o evento no display $displayId"
            accepted
        } catch (t: Throwable) {
            err = "inject: ${t.javaClass.simpleName}: ${t.cause ?: t.message}"
            lastInputErr = err; statExc++
            false
        }
    }

    private class Stream(var active: Boolean = false, var downTime: Long = 0L, var lastX: Float = 0f, var lastY: Float = 0f,
                         var lastTime: Long = 0L, var pointers: Int = 0)
    private val streams = HashMap<Long, Stream>()
    private fun streamKey(displayId: Int, source: Int) = (displayId.toLong() shl 32) or (source.toLong() and 0xFFFFFFFFL)

    private fun clampTime(t: Long, now: Long): Long = if (t <= 0L || t > now + 50L) now else t

    private fun sendSynthetic(displayId: Int, source: Int, action: Int, downTime: Long, x: Float, y: Float, time: Long) {
        val mouse = (source and 0x2002) == 0x2002
        val props = arrayOf(MotionEvent.PointerProperties().apply {
            id = 0; toolType = if (mouse) MotionEvent.TOOL_TYPE_MOUSE else MotionEvent.TOOL_TYPE_FINGER
        })
        val coords = arrayOf(MotionEvent.PointerCoords().apply { this.x = x; this.y = y; pressure = 0f; size = 1f })
        val ev = MotionEvent.obtain(downTime, time, action, 1, props, coords, 0, 0, 1f, 1f, 0, 0, source, 0)
        try { inject(ev, displayId) } finally { ev.recycle() }
    }

    override fun injectMotion(displayId: Int, action: Int, x: Float, y: Float, downTime: Long, eventTime: Long, source: Int): Boolean {
        val mouse = (source and 0x2002) == 0x2002
        val props = arrayOf(MotionEvent.PointerProperties().apply {
            id = 0
            toolType = if (mouse) MotionEvent.TOOL_TYPE_MOUSE else MotionEvent.TOOL_TYPE_FINGER
        })
        val coords = arrayOf(MotionEvent.PointerCoords().apply {
            this.x = x
            this.y = y
            pressure = if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) 0f else 1f
            size = 1f
        })
        val buttons = if (mouse && (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_MOVE))
            MotionEvent.BUTTON_PRIMARY else 0
        val ev = MotionEvent.obtain(
            downTime, eventTime, action, 1, props, coords, 0, buttons,
            1f, 1f, 0, 0, source, 0
        )
        return try { inject(ev, displayId) } finally { ev.recycle() }
    }

    private val inputThread = HandlerThread("plasma-input-svc", Process.THREAD_PRIORITY_URGENT_DISPLAY).also { it.start() }
    private val inputHandler = Handler(inputThread.looper)

    override fun injectTouch(
        displayId: Int, action: Int, actionIndex: Int, source: Int, buttonState: Int, downTime: Long,
        times: LongArray, ids: IntArray, xy: FloatArray,
    ) {
        inputHandler.post {
            try { doInjectTouch(displayId, action, actionIndex, source, buttonState, downTime, times, ids, xy) }
            catch (t: Throwable) { statExc++; err = "injectTouch: ${t.javaClass.simpleName}: ${t.message}"; lastInputErr = err; PlasmaLog.w("shizuku", err) }
        }
    }

    private val probeExec = Executors.newSingleThreadExecutor { r -> Thread(r, "plasma-input-probe").apply { isDaemon = true } }
    private val shellExec = Executors.newSingleThreadExecutor { r -> Thread(r, "plasma-input-shell").apply { isDaemon = true } }
    private val shellSince = HashMap<Int, Long>()
    private val verifiedAt = HashMap<Int, Long>()
    private val rejectStreak = HashMap<Int, Int>()
    @Volatile private var statAsync = 0L
    @Volatile private var statProbeOk = 0L
    @Volatile private var statProbeRej = 0L
    @Volatile private var statProbeTimeout = 0L
    @Volatile private var statShell = 0L
    @Volatile private var statExc = 0L
    @Volatile private var lastInputErr = ""

    override fun inputStats(): String =
        "sdk=${android.os.Build.VERSION.SDK_INT} async=$statAsync probeOk=$statProbeOk probeRej=$statProbeRej " +
            "probeTimeout=$statProbeTimeout shell=$statShell exc=$statExc shellDisplays=${shellSince.keys} err=\"$lastInputErr\""

    private fun injectProbed(ev: MotionEvent, displayId: Int): Boolean? {
        val copy = MotionEvent.obtain(ev)
        val f = probeExec.submit<Boolean> {
            try {
                setDisplayIdMethod.invoke(copy, displayId)
                injectMethod.invoke(inputManager, copy, 1) as? Boolean ?: false
            } finally { copy.recycle() }
        }
        return try { f.get(PLASMA_PROBE_TIMEOUT_MS, TimeUnit.MILLISECONDS) }
        catch (_: TimeoutException) { null }
        catch (t: Throwable) { lastInputErr = "probe: ${t.cause ?: t}"; false }
    }

    private val PLASMA_PROBE_INTERVAL_MS = 30_000L
    private val PLASMA_PROBE_TIMEOUT_MS = 250L
    private val propsBuf = Array(16) { MotionEvent.PointerProperties() }
    private val coordsBuf = Array(16) { MotionEvent.PointerCoords() }

    private class ShellGesture(var sx: Float, var sy: Float, var st: Long, var lx: Float, var ly: Float)
    private val shellGestures = HashMap<Int, ShellGesture>()

    private fun runInput(vararg args: String) {
        shellExec.execute {
            try {
                val p = ProcessBuilder("input", *args).redirectErrorStream(true).start()
                if (!p.waitFor(3, TimeUnit.SECONDS)) p.destroy()
                else if (p.exitValue() != 0) lastInputErr = "input ${args.joinToString(" ")} -> exit ${p.exitValue()}: " +
                    p.inputStream.readBytes().decodeToString().take(120)
                statShell++
            } catch (t: Throwable) { lastInputErr = "input: $t"; statExc++ }
        }
    }

    private fun shellTouch(displayId: Int, action: Int, x: Float, y: Float, time: Long) {
        when (action) {
            MotionEvent.ACTION_DOWN -> shellGestures[displayId] = ShellGesture(x, y, time, x, y)
            MotionEvent.ACTION_MOVE -> shellGestures[displayId]?.let { it.lx = x; it.ly = y }
            MotionEvent.ACTION_UP -> {
                val g = shellGestures.remove(displayId) ?: return
                g.lx = x; g.ly = y
                val dx = g.lx - g.sx; val dy = g.ly - g.sy
                if (dx * dx + dy * dy < 24f * 24f) {
                    runInput("-d", "$displayId", "tap", "${g.sx.toInt()}", "${g.sy.toInt()}")
                } else {
                    val dur = (time - g.st).coerceIn(60L, 1500L)
                    runInput("-d", "$displayId", "swipe", "${g.sx.toInt()}", "${g.sy.toInt()}", "${g.lx.toInt()}", "${g.ly.toInt()}", "$dur")
                }
            }
            MotionEvent.ACTION_CANCEL -> shellGestures.remove(displayId)
        }
    }

    private fun doInjectTouch(
        displayId: Int, action: Int, actionIndex: Int, source: Int, buttonState: Int, downTime: Long,
        times: LongArray, ids: IntArray, xy: FloatArray,
    ) {
        val samples = times.size
        val n = ids.size
        if (samples == 0 || n == 0 || xy.size < samples * n * 2) return
        val now = android.os.SystemClock.uptimeMillis()
        val mouse = (source and 0x2002) == 0x2002
        val hover = action == MotionEvent.ACTION_HOVER_MOVE
        val key = streamKey(displayId, source)

        if (!hover) {
            val st = streams.getOrPut(key) { Stream() }
            when (action) {
                MotionEvent.ACTION_DOWN -> {
                    if (st.active) {

                        sendSynthetic(displayId, source, MotionEvent.ACTION_CANCEL, st.downTime, st.lastX, st.lastY,
                            maxOf(st.lastTime, now))
                    }
                    st.active = true; st.pointers = n
                }
                MotionEvent.ACTION_POINTER_DOWN -> {
                    if (!st.active) return
                    st.pointers = n
                }
                MotionEvent.ACTION_MOVE, MotionEvent.ACTION_POINTER_UP -> if (!st.active) return
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    if (!st.active) return
                    st.active = false; st.pointers = 0
                }
            }
            val k = samples - 1
            st.downTime = clampTime(downTime, now)
            st.lastX = xy[(k * n) * 2]; st.lastY = xy[(k * n) * 2 + 1]; st.lastTime = clampTime(times[k], now)
        }

        shellSince[displayId]?.let { since ->
            if (now - since > 20_000L) { shellSince.remove(displayId); rejectStreak[displayId] = 1; verifiedAt.remove(displayId) }
            else {
                if (!hover) shellTouch(displayId, action and 0xFF, xy[(samples - 1) * n * 2], xy[(samples - 1) * n * 2 + 1], clampTime(times[samples - 1], now))
                return
            }
        }

        val props = if (n <= 16) propsBuf else Array(n) { MotionEvent.PointerProperties() }
        for (i in 0 until n) props[i].apply {
            id = ids[i]
            toolType = if (mouse) MotionEvent.TOOL_TYPE_MOUSE else MotionEvent.TOOL_TYPE_FINGER
        }
        val coordsArr = if (n <= 16) coordsBuf else Array(n) { MotionEvent.PointerCoords() }
        fun coordsAt(k: Int): Array<MotionEvent.PointerCoords> { for (p in 0 until n) coordsArr[p].apply {
                x = xy[(k * n + p) * 2]
                y = xy[(k * n + p) * 2 + 1]

                val leaving = (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) ||
                    (action == MotionEvent.ACTION_POINTER_UP && p == actionIndex)
                pressure = if (hover || leaving) 0f else 1f
                size = 1f
            }
            return coordsArr }
        val a = if (action == MotionEvent.ACTION_POINTER_DOWN || action == MotionEvent.ACTION_POINTER_UP)
            action or (actionIndex shl MotionEvent.ACTION_POINTER_INDEX_SHIFT) else action
        val dt = clampTime(downTime, now)
        val t0 = maxOf(minOf(clampTime(times[0], now), now), dt)
        val ev = MotionEvent.obtain(
            dt, t0, a, n, props, coordsAt(0), 0, buttonState, 1f, 1f, 0, 0, source, 0
        )
        try {

            if (action == MotionEvent.ACTION_MOVE || hover) {
                var prev = t0
                for (k in 1 until samples) {
                    val tk = maxOf(minOf(clampTime(times[k], now), now), prev); prev = tk
                    ev.addBatch(tk, coordsAt(k), 0)
                }
            }
            val needVerify = action == MotionEvent.ACTION_DOWN && !hover && now - (verifiedAt[displayId] ?: 0L) > PLASMA_PROBE_INTERVAL_MS
            if (needVerify) {
                val r = injectProbed(ev, displayId)
                verifiedAt[displayId] = now
                when (r) {
                    true -> { statProbeOk++; rejectStreak[displayId] = 0 }
                    null -> statProbeTimeout++
                    false -> {
                        statProbeRej++
                        lastInputErr = "InputDispatcher recusou o DOWN no display $displayId"
                        PlasmaLog.w("shizuku", lastInputErr)
                        val streak = (rejectStreak[displayId] ?: 0) + 1
                        rejectStreak[displayId] = streak
                        if (streak >= 1) {
                            shellSince[displayId] = now
                            PlasmaLog.w("shizuku", "display $displayId: injeção direta recusada; usando `input -d $displayId`")
                            shellTouch(displayId, MotionEvent.ACTION_DOWN, xy[0], xy[1], now)
                        }
                    }
                }
            } else {
                statAsync++
                inject(ev, displayId)
            }
        } catch (t: Throwable) {
            statExc++; lastInputErr = "touch: ${t.javaClass.simpleName}: ${t.message}"
            PlasmaLog.w("shizuku", lastInputErr)
        } finally { ev.recycle() }
    }

    override fun injectKey(displayId: Int, keyCode: Int, action: Int, meta: Int, downTime: Long, eventTime: Long) {

        inputHandler.post {
            val now = android.os.SystemClock.uptimeMillis()
            val dt = if (downTime <= 0L || downTime > now + 50L) now else downTime
            val et = if (eventTime <= 0L || eventTime > now + 50L) now else eventTime
            inject(KeyEvent(dt, et, action, keyCode, 0, meta, -1, 0, 0, InputDevice.SOURCE_KEYBOARD), displayId)
        }
    }

    override fun injectScroll(displayId: Int, x: Float, y: Float, vScroll: Float, hScroll: Float, time: Long) {
        inputHandler.post { doInjectScroll(displayId, x, y, vScroll, hScroll, time) }
    }

    private fun doInjectScroll(displayId: Int, x: Float, y: Float, vScroll: Float, hScroll: Float, time0: Long) {
        val now = android.os.SystemClock.uptimeMillis()
        val time = if (time0 <= 0L || time0 > now + 50L) now else time0
        val props = arrayOf(MotionEvent.PointerProperties().apply { id = 0; toolType = MotionEvent.TOOL_TYPE_MOUSE })
        val coords = arrayOf(MotionEvent.PointerCoords().apply {
            this.x = x; this.y = y
            setAxisValue(MotionEvent.AXIS_VSCROLL, vScroll); setAxisValue(MotionEvent.AXIS_HSCROLL, hScroll)
        })
        val ev = MotionEvent.obtain(time, time, MotionEvent.ACTION_SCROLL, 1, props, coords, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_MOUSE, 0)
        inject(ev, displayId)
        ev.recycle()
    }
}
