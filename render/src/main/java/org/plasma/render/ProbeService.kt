
package org.plasma.render

import android.app.Service
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.os.Process

const val MSG_PROBE_VK = 1
const val MSG_PROBE_GLES = 2
const val MSG_DIE = 99

class ProbeService : Service() {
    private val messenger = Messenger(Handler(Looper.getMainLooper(), Handler.Callback { msg ->
        when (msg.what) {
            MSG_DIE -> Process.killProcess(Process.myPid())
            else -> {
                val reply = msg.replyTo
                val what = msg.what
                Thread {
                    val text = try {
                        when (what) {
                            MSG_PROBE_VK -> NativeProbe.probeVulkan(true)
                            MSG_PROBE_GLES -> NativeProbe.probeGles(true)
                            else -> "error=mensagem desconhecida"
                        }
                    } catch (t: Throwable) { "error=$t" }
                    try {
                        reply?.send(Message.obtain(null, what).apply { data = Bundle().apply { putString("r", text) } })
                    } catch (_: Exception) {}
                }.start()
            }
        }
        true
    }))

    override fun onBind(intent: Intent?): IBinder = messenger.binder
}
