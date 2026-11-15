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

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.Surface
import org.plasma.core.PlasmaLog

class DesktopEncoder(
    private val cfg: StreamConfig,
    private val onPacket: (data: ByteArray, key: Boolean, ptsUs: Long, config: Boolean) -> Unit,
    private val onError: (String) -> Unit,
) {
    private var codec: MediaCodec? = null
    private var surface: Surface? = null
    @Volatile private var running = false
    private val ui = Handler(Looper.getMainLooper())
    private var drain: Thread? = null
    @Volatile var framesEncoded = 0L; private set
    @Volatile var framesSkipped = 0L; private set
    @Volatile var lastConfig: ByteArray? = null; private set

    fun start(source: FrameSource) {
        val fmt = MediaFormat.createVideoFormat(cfg.codec.mime, cfg.width, cfg.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, cfg.bitrateKbps * 1000)
            setInteger(MediaFormat.KEY_FRAME_RATE, cfg.fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            if (cfg.lowLatency && Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
        }
        val c = MediaCodec.createEncoderByType(cfg.codec.mime)
        try {
            c.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        } catch (t: Throwable) {
            PlasmaLog.w("encoder", "configure falhou; tentando sem CBR/low-latency", t)
            fmt.removeKey(MediaFormat.KEY_BITRATE_MODE)
            if (Build.VERSION.SDK_INT >= 30) fmt.removeKey(MediaFormat.KEY_LOW_LATENCY)
            c.reset()
            c.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        }
        surface = c.createInputSurface()
        c.start()
        codec = c
        running = true
        PlasmaLog.i("encoder", "encoder ${c.name} ${cfg.width}x${cfg.height}@${cfg.fps} ${cfg.bitrateKbps}kbps ${cfg.codec.label}")

        drain = Thread({
            val info = MediaCodec.BufferInfo()
            while (running) {
                try {
                    val i = c.dequeueOutputBuffer(info, 100_000)
                    if (i >= 0) {
                        val buf = c.getOutputBuffer(i)
                        if (buf != null && info.size > 0) {
                            val bytes = ByteArray(info.size)
                            buf.position(info.offset); buf.limit(info.offset + info.size); buf.get(bytes)
                            val isCfg = info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0
                            val key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                            if (isCfg) lastConfig = bytes else framesEncoded++
                            onPacket(bytes, key, info.presentationTimeUs, isCfg)
                        }
                        c.releaseOutputBuffer(i, false)
                    }
                } catch (t: Throwable) {
                    if (running) { PlasmaLog.e("encoder", "drain falhou", t); onError(t.toString()) }
                    break
                }
            }
        }, "plasma-encoder-drain").also { it.start() }

        val period = 1000L / cfg.fps.coerceAtLeast(1)
        var lastPost = 0L
        val pump = object : Runnable {
            override fun run() {
                if (!running) return
                val now = SystemClock.uptimeMillis()
                val dirty = source.consumeDirty()
                if (dirty || now - lastPost >= 200) {
                    try {
                        val s = surface!!
                        val canvas = s.lockHardwareCanvas()
                        try { source.draw(canvas, cfg.width, cfg.height) } finally { s.unlockCanvasAndPost(canvas) }
                        lastPost = now
                    } catch (t: Throwable) {
                        PlasmaLog.e("encoder", "desenho falhou", t); onError(t.toString()); running = false; return
                    }
                } else framesSkipped++
                ui.postDelayed(this, period)
            }
        }
        ui.post(pump)
    }

    fun requestKeyFrame() {
        runCatching { codec?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) }) }
    }

    fun stop() {
        running = false
        ui.removeCallbacksAndMessages(null)
        runCatching { drain?.join(500) }
        runCatching { codec?.stop() }
        runCatching { codec?.release() }
        runCatching { surface?.release() }
        codec = null; surface = null
    }
}
