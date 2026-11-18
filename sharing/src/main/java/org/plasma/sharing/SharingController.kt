
package org.plasma.sharing

import android.content.Context
import kotlinx.coroutines.flow.StateFlow
import org.plasma.core.Keys
import org.plasma.core.SettingsStore

class SharingController(ctx: Context, private val settings: SettingsStore) {
    private val server = SharingServer(ctx)
    val stats: StateFlow<SharingStats> get() = server.stats

    fun config() = StreamConfig(
        width = settings.int(Keys.SHARE_W, 1280), height = settings.int(Keys.SHARE_H, 720),
        fps = settings.int(Keys.SHARE_FPS, 30), bitrateKbps = settings.int(Keys.SHARE_KBPS, 6000),
        codec = VideoCodec.of(settings.int(Keys.SHARE_CODEC, 1)), lowLatency = settings.bool(Keys.SHARE_LOWLAT, true),
        pin = settings.string(Keys.SHARE_PIN, "1234"), port = settings.int(Keys.SHARE_PORT, 47800),
    )

    fun start(src: FrameSource): String? = server.start(config(), src)
    fun stop() = server.stop()
}
