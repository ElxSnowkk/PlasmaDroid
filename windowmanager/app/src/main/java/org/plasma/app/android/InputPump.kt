
package org.plasma.app.android

import org.plasma.bridge.PrivilegedBackend
import java.util.concurrent.Executors

object InputPump {
    private val exec = Executors.newSingleThreadExecutor { r -> Thread({ android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY); r.run() }, "plasma-input").apply { isDaemon = true } }
    private val lock = Any()

    private val queue = java.util.ArrayDeque<Any>()
    private var draining = false
    private const val MAX_SAMPLES = 96
    private const val MAX_QUEUED = 512

    private class MoveBatch(
        val be: PrivilegedBackend, val display: Int, val action: Int, val mouse: Boolean,
        val buttons: Int, val downTime: Long, val ids: IntArray,
    ) {
        var times = LongArray(16); var xy = FloatArray(16 * ids.size * 2); var count = 0
        fun append(t: LongArray, p: FloatArray) {
            val add = t.size
            if (count + add > times.size) {
                val cap = maxOf(times.size * 2, count + add)
                times = times.copyOf(cap); xy = xy.copyOf(cap * ids.size * 2)
            }
            System.arraycopy(t, 0, times, count, add)
            System.arraycopy(p, 0, xy, count * ids.size * 2, add * ids.size * 2)
            count += add
        }
        fun compatible(be: PrivilegedBackend, display: Int, action: Int, buttons: Int, downTime: Long, ids: IntArray) =
            this.be === be && this.display == display && this.action == action && this.buttons == buttons &&
                this.downTime == downTime && this.ids.contentEquals(ids) && count < MAX_SAMPLES
        fun send() {
            if (count == 0) return
            be.touch(display, action, 0, mouse, buttons, downTime, times.copyOf(count), ids, xy.copyOf(count * ids.size * 2))
        }
    }

    private fun kickLocked() {
        if (draining) return
        draining = true
        exec.execute { drain() }
    }

    private fun drain() {
        while (true) {
            val item = synchronized(lock) { queue.pollFirst() ?: run { draining = false; return } }
            runCatching {
                when (item) {
                    is MoveBatch -> item.send()
                    is Runnable -> item.run()
                }
            }
        }
    }

    fun post(r: Runnable) {
        synchronized(lock) { queue.addLast(r); kickLocked() }
    }

    fun move(be: PrivilegedBackend, display: Int, action: Int, mouse: Boolean, buttons: Int, downTime: Long,
             ids: IntArray, times: LongArray, xy: FloatArray) {
        synchronized(lock) {
            val last = queue.peekLast()
            if (last is MoveBatch && last.compatible(be, display, action, buttons, downTime, ids)) {
                last.append(times, xy)
            } else {

                if (queue.size > MAX_QUEUED) queue.removeAll { it is MoveBatch }
                queue.addLast(MoveBatch(be, display, action, mouse, buttons, downTime, ids.copyOf()).also { it.append(times, xy) })
            }
            kickLocked()
        }
    }
}
