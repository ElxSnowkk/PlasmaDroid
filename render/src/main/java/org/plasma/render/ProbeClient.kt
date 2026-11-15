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

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.withTimeoutOrNull

class ProbeClient(private val ctx: Context) {
    suspend fun run(what: Int, timeoutMs: Long = 6000): Result<Map<String, String>> {
        val result = CompletableDeferred<Result<String>>()
        val replyMessenger = Messenger(Handler(Looper.getMainLooper(), Handler.Callback { m ->
            result.complete(Result.success(m.data.getString("r") ?: "")); true
        }))
        var service: Messenger? = null
        val conn = object : ServiceConnection {
            override fun onServiceConnected(n: ComponentName, b: IBinder) {
                service = Messenger(b)
                try {
                    service!!.send(Message.obtain(null, what).also { it.replyTo = replyMessenger })
                } catch (e: Exception) { result.complete(Result.failure(e)) }
            }
            override fun onServiceDisconnected(n: ComponentName) {
                result.complete(Result.failure(IllegalStateException("processo da sonda morreu (provavel crash do driver)")))
            }
            override fun onBindingDied(n: ComponentName) = onServiceDisconnected(n)
        }
        if (!ctx.bindService(Intent(ctx, ProbeService::class.java), conn, Context.BIND_AUTO_CREATE))
            return Result.failure(IllegalStateException("bindService falhou"))
        val r = withTimeoutOrNull(timeoutMs) { result.await() }
        if (r == null) runCatching { service?.send(Message.obtain(null, MSG_DIE)) }
        runCatching { ctx.unbindService(conn) }
        return when {
            r == null -> Result.failure(IllegalStateException("timeout de ${timeoutMs}ms (driver travado?)"))
            r.isFailure -> Result.failure(r.exceptionOrNull()!!)
            else -> Result.success(parse(r.getOrThrow()))
        }
    }

    companion object {
        fun parse(text: String): Map<String, String> =
            text.lineSequence().mapNotNull { l -> l.indexOf('=').takeIf { it > 0 }?.let { l.substring(0, it) to l.substring(it + 1) } }.toMap()
    }
}
