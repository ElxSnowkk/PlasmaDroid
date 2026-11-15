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

import android.graphics.Canvas
import android.media.MediaCodecInfo
import android.media.MediaCodecList

enum class VideoCodec(val id: Int, val mime: String, val label: String) {
    H264(1, "video/avc", "H.264"), H265(2, "video/hevc", "H.265 (HEVC)"), AV1(3, "video/av01", "AV1 (experimental)");
    companion object { fun of(id: Int) = entries.firstOrNull { it.id == id } ?: H264 }
}

data class StreamConfig(
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrateKbps: Int,
    val codec: VideoCodec,
    val lowLatency: Boolean,
    val pin: String,
    val port: Int,
)

data class SharingStats(
    val running: Boolean = false,
    val clients: Int = 0,
    val fps: Float = 0f,
    val kbps: Int = 0,
    val framesEncoded: Long = 0,
    val framesDropped: Long = 0,
    val bytesSent: Long = 0,
    val lastError: String = "",
)

interface FrameSource {
    fun draw(canvas: Canvas, w: Int, h: Int)
    fun consumeDirty(): Boolean
}

data class CodecSupport(val codec: VideoCodec, val encoderName: String?, val hardware: Boolean)

object CodecCaps {
    fun query(): List<CodecSupport> {
        val infos = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { it.isEncoder }
        return VideoCodec.entries.map { c ->
            val cands = infos.filter { i -> i.supportedTypes.any { it.equals(c.mime, true) } }
            val best = cands.firstOrNull { it.isHardwareAccelerated } ?: cands.firstOrNull()
            CodecSupport(c, best?.name, best?.isHardwareAccelerated == true)
        }
    }
    fun isAvailable(c: VideoCodec) = query().first { it.codec == c }.encoderName != null
}
