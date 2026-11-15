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

package org.plasma.sharing

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.SystemClock
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.plasma.core.PlasmaLog
import java.io.BufferedOutputStream
import java.io.DataOutputStream
import java.net.ServerSocket
import java.net.Socket
import java.security.SecureRandom
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicLong
import javax.crypto.SecretKey

class SharingServer(private val ctx: Context) {
    private val _stats = MutableStateFlow(SharingStats())
    val stats: StateFlow<SharingStats> = _stats.asStateFlow()

    private class Client(val socket: Socket, val key: SecretKey) {
        val out = DataOutputStream(BufferedOutputStream(socket.getOutputStream(), 64 * 1024))
        val queue = LinkedBlockingQueue<ByteArray>(90)
        val counter = AtomicLong(0)
        @Volatile var alive = true
        var needKey = true
    }

    private val clients = CopyOnWriteArrayList<Client>()
    private var server: ServerSocket? = null
    private var encoder: DesktopEncoder? = null
    private var cfg: StreamConfig? = null
    private var source: FrameSource? = null
    private var salt = ByteArray(16)
    private var masterKey: SecretKey? = null
    @Volatile private var running = false
    private val sent = AtomicLong(0)
    private val dropped = AtomicLong(0)
    private var nsd: NsdManager? = null
    private var nsdListener: NsdManager.RegistrationListener? = null

    @Synchronized
    fun start(config: StreamConfig, src: FrameSource): String? {
        if (running) return null
        if (config.pin.length < 4) return "O PIN precisa ter ao menos 4 caracteres"
        if (!CodecCaps.isAvailable(config.codec)) return "Este aparelho não tem encoder ${config.codec.label}"
        return try {
            cfg = config; source = src
            salt = ByteArray(16).also { SecureRandom().nextBytes(it) }
            masterKey = Crypto.key(config.pin, salt)
            server = ServerSocket(config.port).also { it.reuseAddress = true }
            running = true; sent.set(0); dropped.set(0)
            Thread(::acceptLoop, "plasma-share-accept").start()
            Thread(::statsLoop, "plasma-share-stats").start()
            registerNsd(config.port)
            PlasmaLog.i("sharing", "servidor ouvindo na porta ${config.port}")
            publish(error = "")
            null
        } catch (t: Throwable) {
            PlasmaLog.e("sharing", "falha ao iniciar", t); running = false; t.message ?: t.toString()
        }
    }

    @Synchronized
    fun stop() {
        if (!running) return
        running = false
        runCatching { server?.close() }
        clients.forEach { close(it) }; clients.clear()
        encoder?.stop(); encoder = null
        unregisterNsd()
        _stats.value = SharingStats()
        PlasmaLog.i("sharing", "servidor parado")
    }

    private fun acceptLoop() {
        while (running) {
            try {
                val s = server?.accept() ?: break
                s.tcpNoDelay = true
                val c = Client(s, masterKey!!)
                val cf = cfg!!
                val h = DataOutputStream(s.getOutputStream())
                h.write("KPAD".toByteArray()); h.writeByte(1); h.writeByte(cf.codec.id)
                h.writeShort(cf.width); h.writeShort(cf.height); h.writeByte(cf.fps); h.write(salt); h.flush()
                clients.add(c)
                PlasmaLog.i("sharing", "cliente conectado: ${s.inetAddress.hostAddress}")
                Thread({ writerLoop(c) }, "plasma-share-writer").start()
                onClientsChanged()
            } catch (t: Throwable) { if (running) PlasmaLog.w("sharing", "accept falhou", t) }
        }
    }

    private fun writerLoop(c: Client) {
        try {
            while (running && c.alive) {
                val pkt = c.queue.poll(500, java.util.concurrent.TimeUnit.MILLISECONDS) ?: continue
                c.out.write(pkt); c.out.flush()
                sent.addAndGet(pkt.size.toLong())
            }
        } catch (t: Throwable) { PlasmaLog.i("sharing", "cliente desconectado: ${t.message}") }
        close(c); clients.remove(c); onClientsChanged()
    }

    private fun close(c: Client) { c.alive = false; runCatching { c.socket.close() } }

    private fun onClientsChanged() {
        synchronized(this) {
            if (!running) return
            if (clients.isNotEmpty() && encoder == null) {
                val cf = cfg!!
                val e = DesktopEncoder(cf, ::onPacket) { err -> publish(error = err) }
                try { e.start(source!!); encoder = e } catch (t: Throwable) {
                    PlasmaLog.e("sharing", "encoder falhou", t); publish(error = t.message ?: t.toString()); clients.forEach { close(it) }
                }
            } else if (clients.isEmpty() && encoder != null) {
                encoder?.stop(); encoder = null
            } else encoder?.requestKeyFrame()
            publish()
        }
    }

    private fun onPacket(data: ByteArray, key: Boolean, pts: Long, config: Boolean) {
        val flags = (if (key) 1 else 0) or (if (config) 2 else 0)
        for (c in clients) {
            if (!c.alive) continue
            if (c.needKey) {
                if (config) {  }
                else if (!key) { dropped.incrementAndGet(); continue } else c.needKey = false
                if (!config) encoder?.lastConfig?.let { cfgBytes -> c.queue.offer(packet(c, 2, 0, cfgBytes)) }
            }
            val pkt = packet(c, flags, pts, data)
            if (!c.queue.offer(pkt)) {
                dropped.addAndGet(c.queue.size.toLong() + 1)
                c.queue.clear(); c.needKey = true; encoder?.requestKeyFrame()
            }
        }
    }

    private fun packet(c: Client, flags: Int, pts: Long, data: ByteArray): ByteArray {
        val plain = java.nio.ByteBuffer.allocate(9 + data.size).put(flags.toByte()).putLong(pts).put(data).array()
        val ctr = c.counter.getAndIncrement()
        val enc = Crypto.seal(c.key, ctr, plain)
        return java.nio.ByteBuffer.allocate(4 + 8 + enc.size).putInt(8 + enc.size).putLong(ctr).put(enc).array()
    }

    private fun statsLoop() {
        var lastBytes = 0L; var lastFrames = 0L; var lastT = SystemClock.elapsedRealtime()
        while (running) {
            try { Thread.sleep(1000) } catch (_: InterruptedException) { break }
            val now = SystemClock.elapsedRealtime(); val dt = (now - lastT).coerceAtLeast(1) / 1000f
            val b = sent.get(); val f = encoder?.framesEncoded ?: 0L
            val kbps = ((b - lastBytes) * 8 / 1000 / dt).toInt()
            val fps = (f - lastFrames) / dt
            lastBytes = b; lastFrames = f; lastT = now
            publish(fps = fps, kbps = kbps)
        }
    }

    private fun publish(fps: Float? = null, kbps: Int? = null, error: String? = null) {
        _stats.value = _stats.value.copy(
            running = running, clients = clients.size,
            fps = fps ?: _stats.value.fps, kbps = kbps ?: _stats.value.kbps,
            framesEncoded = encoder?.framesEncoded ?: _stats.value.framesEncoded,
            framesDropped = dropped.get(), bytesSent = sent.get(),
            lastError = error ?: _stats.value.lastError,
        )
    }

    private fun registerNsd(port: Int) {
        try {
            val m = ctx.getSystemService(Context.NSD_SERVICE) as NsdManager
            val info = NsdServiceInfo().apply { serviceName = "KDE Plasma Android Desktop"; serviceType = "_plasmadesk._tcp."; setPort(port) }
            val l = object : NsdManager.RegistrationListener {
                override fun onServiceRegistered(i: NsdServiceInfo) { PlasmaLog.i("sharing", "NSD registrado: ${i.serviceName}") }
                override fun onRegistrationFailed(i: NsdServiceInfo, e: Int) { PlasmaLog.w("sharing", "NSD falhou: $e") }
                override fun onServiceUnregistered(i: NsdServiceInfo) {}
                override fun onUnregistrationFailed(i: NsdServiceInfo, e: Int) {}
            }
            m.registerService(info, NsdManager.PROTOCOL_DNS_SD, l)
            nsd = m; nsdListener = l
        } catch (t: Throwable) { PlasmaLog.w("sharing", "NSD indisponível", t) }
    }

    private fun unregisterNsd() { runCatching { nsdListener?.let { nsd?.unregisterService(it) } }; nsd = null; nsdListener = null }
}
