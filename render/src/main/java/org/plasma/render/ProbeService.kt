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
