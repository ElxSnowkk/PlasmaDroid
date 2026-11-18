
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
