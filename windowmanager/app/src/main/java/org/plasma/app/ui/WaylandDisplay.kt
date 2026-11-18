
package org.plasma.app.ui

import android.graphics.PixelFormat
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.unit.IntSize
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.rememberUpdatedState
import org.plasma.core.Keys
import org.plasma.app.session.DesktopSession

private const val FIXED_W = 1280
private const val FIXED_H = 720

@Composable
fun WaylandDisplayContent(session: DesktopSession, desktopPx: IntSize = IntSize.Zero) {
    val manager = session.app.wayland
    val sync = session.app.settings.bool(Keys.WAYLAND_SYNC_SIZE, false)
    val syncState = rememberUpdatedState(sync)
    val sizeState = rememberUpdatedState(desktopPx)

    LaunchedEffect(sync) { if (!sync) manager.requestOutputSize(FIXED_W, FIXED_H) }
    LaunchedEffect(sync, desktopPx) {
        if (sync && desktopPx.width > 0 && desktopPx.height > 0)
            manager.requestOutputSize(desktopPx.width, desktopPx.height)
    }

    AndroidView(
        modifier = Modifier.size(1.dp).alpha(0.01f),
        factory = { context ->
            SurfaceView(context).apply {
                holder.setFormat(PixelFormat.OPAQUE)
                holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceCreated(holder: SurfaceHolder) {
                        val d = sizeState.value
                        val (w, h) = if (syncState.value && d.width > 0 && d.height > 0)
                            d.width to d.height else FIXED_W to FIXED_H
                        manager.attachSurface(holder.surface, w, h)
                    }
                    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                        if (!syncState.value) manager.requestOutputSize(FIXED_W, FIXED_H)
                    }
                    override fun surfaceDestroyed(holder: SurfaceHolder) {
                        manager.detachSurface()
                    }
                })
            }
        },
        update = { },
    )
}
